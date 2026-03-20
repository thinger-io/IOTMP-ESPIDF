// The MIT License (MIT)
//
// Copyright (c) INTERNET OF THINGER SL
// Author: alvarolb@gmail.com (Alvaro Luis Bustamante)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <thinger/iotmp/client.hpp>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include <cerrno>
#include <cstring>
#include <algorithm>

static const char* TAG = "iotmp";

namespace thinger::iotmp {

// ============================================================================
// Helper: monotonic uptime in milliseconds
// ============================================================================

static inline int64_t uptime_ms() {
    return esp_timer_get_time() / 1000;
}

// ============================================================================
// Construction / Configuration
// ============================================================================

client::client() {
    tx_mutex_ = xSemaphoreCreateMutex();
}

client::~client() {
    stop();
    if(tx_mutex_) {
        vSemaphoreDelete(tx_mutex_);
        tx_mutex_ = nullptr;
    }
}

void client::set_credentials(const char* username, const char* device_id, const char* credentials) {
    username_ = username;
    device_id_ = device_id;
    credentials_ = credentials;
}

void client::set_host(const char* host, uint16_t port) {
    host_ = host;
    if(port != 0) {
        port_ = port;
    }
}

iotmp_resource& client::operator[](const char* name) {
    return resources_[std::string(name)];
}

// ============================================================================
// Lifecycle
// ============================================================================

esp_err_t client::start() {
    if(running_) return ESP_ERR_INVALID_STATE;

    running_ = true;

    BaseType_t ret = xTaskCreate(
        task_entry,
        "iotmp",
        CONFIG_THINGER_IOTMP_STACK_SIZE,
        this,
        CONFIG_THINGER_IOTMP_PRIORITY,
        &task_handle_
    );

    if(ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        running_ = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "IOTMP client started");
    return ESP_OK;
}

void client::stop() {
    if(!running_) return;

    running_ = false;

    // Notify the task so it wakes from any wait
    if(task_handle_) {
        xTaskNotifyGive(task_handle_);
    }

    // Wait for task to finish (give it some time)
    if(task_handle_) {
        // Busy-wait for task completion with timeout
        int timeout = 100; // 10 seconds (100 * 100ms)
        while(eTaskGetState(task_handle_) != eDeleted && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout--;
        }
        task_handle_ = nullptr;
    }

    do_disconnect();

    ESP_LOGI(TAG, "IOTMP client stopped");
}

// ============================================================================
// Task entry
// ============================================================================

void client::task_entry(void* arg) {
    auto* self = static_cast<client*>(arg);
    self->run();
    vTaskDelete(nullptr);
}

void client::run() {
    int backoff_ms = CONFIG_THINGER_IOTMP_RECONNECT_BASE_MS;

    while(running_) {
        // Connect
        notify_state(client_state::CONNECTING);
        int rc = do_connect();
        if(rc < 0) {
            ESP_LOGE(TAG, "Connection failed: %d", rc);
            notify_state(client_state::CONNECTION_ERROR);
            goto reconnect;
        }
        notify_state(client_state::CONNECTED);

        // Authenticate
        notify_state(client_state::AUTHENTICATING);
        if(!do_authenticate()) {
            ESP_LOGE(TAG, "Authentication failed");
            notify_state(client_state::AUTH_FAILED);
            do_disconnect();
            goto reconnect;
        }

        ESP_LOGI(TAG, "Authenticated as %s@%s", device_id_.c_str(), username_.c_str());
        connected_ = true;
        notify_state(client_state::AUTHENTICATED);
        backoff_ms = CONFIG_THINGER_IOTMP_RECONNECT_BASE_MS;
        notify_state(client_state::READY);

        // Event loop
        {
            int64_t last_activity = uptime_ms();

            while(running_ && connected_) {
                int64_t elapsed = uptime_ms() - last_activity;
                int keepalive_ms = CONFIG_THINGER_IOTMP_KEEPALIVE_SECONDS * 1000;
                int timeout_ms = std::max(0, static_cast<int>(keepalive_ms - elapsed));

                // Also check stream intervals
                int stream_timeout = 1000; // Check streams every second
                timeout_ms = std::min(timeout_ms, stream_timeout);

                // Cap at 100ms so we can check the TX queue frequently
                timeout_ms = std::min(timeout_ms, 100);

                fd_set read_fds;
                FD_ZERO(&read_fds);

                int max_fd = -1;

#ifdef CONFIG_THINGER_IOTMP_TLS
                // For TLS, we need to get the underlying socket fd
                int tls_fd = -1;
                if(tls_) {
                    esp_tls_get_conn_sockfd(tls_, &tls_fd);
                    if(tls_fd >= 0) {
                        FD_SET(tls_fd, &read_fds);
                        if(tls_fd > max_fd) max_fd = tls_fd;
                    }
                }
#else
                FD_SET(sock_, &read_fds);
                if(sock_ > max_fd) max_fd = sock_;
#endif

                struct timeval tv;
                tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;

                rc = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);

                if(rc < 0) {
                    ESP_LOGE(TAG, "select error: %d", errno);
                    break;
                }

                // Incoming data on socket
                bool socket_readable = false;
#ifdef CONFIG_THINGER_IOTMP_TLS
                if(tls_fd >= 0 && FD_ISSET(tls_fd, &read_fds)) {
                    socket_readable = true;
                }
                // Also check if TLS has buffered data
                if(tls_ && esp_tls_get_bytes_avail(tls_) > 0) {
                    socket_readable = true;
                }
#else
                if(FD_ISSET(sock_, &read_fds)) {
                    socket_readable = true;
                }
#endif

                if(socket_readable) {
                    iotmp_message msg(message::type::RESERVED);
                    if(read_message(msg)) {
                        handle_message(msg);
                        last_activity = uptime_ms();
                    } else {
                        ESP_LOGW(TAG, "Failed to read message, disconnecting");
                        break;
                    }
                }

                // Always check TX queue (replaces wakeup pipe)
                if(!running_) break;
                flush_tx_queue();

                // Keepalive timeout
                elapsed = uptime_ms() - last_activity;
                if(elapsed >= keepalive_ms) {
                    auto ka = encode_message(message::type::KEEP_ALIVE);
                    socket_write(ka.data(), ka.size());
                    last_activity = uptime_ms();
                    ESP_LOGD(TAG, "Keep-alive sent");
                }

                // Check stream intervals
                check_stream_intervals();
            }
        }

        connected_ = false;
        notify_state(client_state::DISCONNECTED);
        do_disconnect();
        streams_.clear();

reconnect:
        if(!running_) break;

        ESP_LOGI(TAG, "Reconnecting in %d ms...", backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));

        // Exponential backoff with cap
        backoff_ms = std::min(backoff_ms * 2, CONFIG_THINGER_IOTMP_RECONNECT_MAX_MS);
    }
}

// ============================================================================
// Connection
// ============================================================================

int client::do_connect() {
    ESP_LOGI(TAG, "Connecting to %s:%d...", host_.c_str(), port_);

#ifdef CONFIG_THINGER_IOTMP_TLS
    // Use esp_tls for TLS connections
    esp_tls_cfg_t cfg = {};
    cfg.timeout_ms = 10000;
    cfg.non_block = false;
    // Use system certificate bundle if available, otherwise skip verification
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    tls_ = esp_tls_init();
    if(!tls_) {
        ESP_LOGE(TAG, "Failed to allocate esp_tls handle");
        return -1;
    }

    int ret = esp_tls_conn_new_sync(host_.c_str(), host_.size(), port_, &cfg, tls_);
    if(ret != 1) {
        ESP_LOGE(TAG, "TLS connection failed to %s:%d", host_.c_str(), port_);
        esp_tls_conn_destroy(tls_);
        tls_ = nullptr;
        return -1;
    }

    ESP_LOGI(TAG, "Connected to %s:%d (TLS)", host_.c_str(), port_);
#else
    // Plain TCP using BSD sockets
    struct addrinfo hints = {};
    struct addrinfo* res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port_);

    int rc = getaddrinfo(host_.c_str(), port_str, &hints, &res);
    if(rc != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolve failed for %s: %d", host_.c_str(), rc);
        return -1;
    }

    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if(sock_ < 0) {
        ESP_LOGE(TAG, "Socket creation failed: %d", errno);
        freeaddrinfo(res);
        return -errno;
    }

    // Set receive timeout
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // TCP_NODELAY
    int flag = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    rc = connect(sock_, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if(rc < 0) {
        ESP_LOGE(TAG, "Connect failed: %d", errno);
        close(sock_);
        sock_ = -1;
        return -errno;
    }

    ESP_LOGI(TAG, "Connected to %s:%d (TCP)", host_.c_str(), port_);
#endif

    return 0;
}

void client::do_disconnect() {
#ifdef CONFIG_THINGER_IOTMP_TLS
    if(tls_) {
        esp_tls_conn_destroy(tls_);
        tls_ = nullptr;
    }
#else
    if(sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
#endif
    connected_ = false;
}

// ============================================================================
// Authentication
// ============================================================================

bool client::do_authenticate() {
    // IMPORTANT: Credentials are sent in PAYLOAD as array [username, device_id, credential]
    iotmp_message connect_msg(message::type::CONNECT);
    connect_msg.set_random_stream_id();
    connect_msg[message::field::PAYLOAD] = json_t::array({username_, device_id_, credentials_});

    if(!write_message(connect_msg)) {
        ESP_LOGE(TAG, "Failed to send CONNECT");
        return false;
    }

    iotmp_message response(message::type::RESERVED);
    if(!read_message(response)) {
        ESP_LOGE(TAG, "No CONNECT response");
        return false;
    }

    return response.get_message_type() == message::type::OK;
}

// ============================================================================
// Socket I/O
// ============================================================================

bool client::socket_read(void* buf, size_t len) {
    auto* ptr = static_cast<uint8_t*>(buf);
    size_t remaining = len;

    while(remaining > 0) {
#ifdef CONFIG_THINGER_IOTMP_TLS
        if(!tls_) return false;
        ssize_t rc = esp_tls_conn_read(tls_, ptr, remaining);
        if(rc < 0) {
            if(rc == ESP_TLS_ERR_SSL_WANT_READ || rc == ESP_TLS_ERR_SSL_WANT_WRITE) {
                continue; // Retry on TLS want read/write
            }
            return false;
        }
        if(rc == 0) return false; // Connection closed
#else
        ssize_t rc = recv(sock_, ptr, remaining, 0);
        if(rc <= 0) return false;
#endif
        ptr += rc;
        remaining -= rc;
    }
    return true;
}

bool client::socket_write(const void* buf, size_t len) {
    auto* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;

    while(remaining > 0) {
#ifdef CONFIG_THINGER_IOTMP_TLS
        if(!tls_) return false;
        ssize_t rc = esp_tls_conn_write(tls_, ptr, remaining);
        if(rc < 0) {
            if(rc == ESP_TLS_ERR_SSL_WANT_READ || rc == ESP_TLS_ERR_SSL_WANT_WRITE) {
                continue; // Retry on TLS want read/write
            }
            return false;
        }
        if(rc == 0) return false;
#else
        ssize_t rc = send(sock_, ptr, remaining, 0);
        if(rc <= 0) return false;
#endif
        ptr += rc;
        remaining -= rc;
    }
    return true;
}

bool client::read_varint(uint32_t& value) {
    value = 0;
    uint8_t byte;
    uint8_t bit_pos = 0;

    do {
        if(!socket_read(&byte, 1) || bit_pos >= 32) return false;
        value |= static_cast<uint32_t>(byte & 0x7F) << bit_pos;
        bit_pos += 7;
    } while(byte & 0x80);

    return true;
}

// ============================================================================
// Message I/O
// ============================================================================

bool client::read_message(iotmp_message& msg) {
    // Read message type (1 byte varint)
    uint32_t type_val;
    if(!read_varint(type_val)) return false;

    // Read body size
    uint32_t body_size;
    if(!read_varint(body_size)) return false;

    msg.set_message_type(static_cast<message::type>(type_val));

    if(body_size == 0) return true;

    if(body_size > CONFIG_THINGER_IOTMP_MAX_MESSAGE_SIZE) {
        ESP_LOGE(TAG, "Message too large: %u bytes", (unsigned)body_size);
        return false;
    }

    // Read body
    if(!socket_read(read_buffer_, body_size)) return false;

    // Decode fields
    memory_reader reader(read_buffer_, body_size);
    iotmp_decoder<memory_reader> decoder(reader);
    return decoder.decode(msg, body_size);
}

bool client::write_message(iotmp_message& msg) {
    auto encoded = encode_message(msg);
    return socket_write(encoded.data(), encoded.size());
}

void client::send_message(iotmp_message& msg) {
    if(!connected_) return;

    if(msg.get_message_type() != message::STREAM_DATA) {
        ESP_LOGD(TAG, "TX: %s (stream=%u)", msg.message_type_str(), msg.get_stream_id());
    }

    write_message(msg);
}

bool client::enqueue_message(iotmp_message& msg) {
    auto encoded = encode_message(msg);

    xSemaphoreTake(tx_mutex_, portMAX_DELAY);
    tx_queue_.push(std::move(encoded));
    xSemaphoreGive(tx_mutex_);

    // Notify the client task so it picks up queued data promptly
    if(task_handle_) {
        xTaskNotifyGive(task_handle_);
    }
    return true;
}

void client::flush_tx_queue() {
    xSemaphoreTake(tx_mutex_, portMAX_DELAY);
    while(!tx_queue_.empty()) {
        auto& data = tx_queue_.front();
        socket_write(data.data(), data.size());
        tx_queue_.pop();
    }
    xSemaphoreGive(tx_mutex_);
}

// ============================================================================
// Message handling
// ============================================================================

void client::handle_message(iotmp_message& msg) {
    if(msg.get_message_type() != message::STREAM_DATA) {
        ESP_LOGD(TAG, "RX: %s (stream=%u)", msg.message_type_str(), msg.get_stream_id());
    }

    switch(msg.get_message_type()) {
        case message::KEEP_ALIVE:
            ESP_LOGD(TAG, "Keep-alive received");
            break;

        case message::RUN:
        case message::DESCRIBE:
        case message::START_STREAM:
        case message::STOP_STREAM:
        case message::STREAM_DATA:
            handle_resource_request(msg);
            break;

        default:
            ESP_LOGW(TAG, "Unhandled message type: %d", static_cast<int>(msg.get_message_type()));
            break;
    }
}

void client::handle_resource_request(iotmp_message& request) {
    iotmp_resource* resource = nullptr;

    auto msg_type = request.get_message_type();

    // For stream data and stop, look up by stream ID
    if(msg_type == message::STREAM_DATA || msg_type == message::STOP_STREAM) {
        auto it = streams_.find(request.get_stream_id());
        if(it != streams_.end()) {
            resource = it->second.resource;
        }
    }

    // Look up by resource path
    if(!resource && request.has_field(message::field::RESOURCE)) {
        const auto& res = request[message::field::RESOURCE];
        if(res.is_string()) {
            resource = find_resource(res.get<std::string>());
        }
    }

    if(!resource) {
        // Handle DESCRIBE without resource (API listing)
        if(msg_type == message::DESCRIBE && !request.has_field(message::field::RESOURCE)) {
            iotmp_message response(request.get_stream_id(), message::type::OK);
            for(auto& [path, res] : resources_) {
                res.fill_api(response[message::field::PAYLOAD][path.c_str()]);
            }
            send_message(response);
            return;
        }

        if(msg_type != message::STREAM_DATA) {
            iotmp_message error(request.get_stream_id(), message::type::ERROR);
            send_message(error);
        }
        return;
    }

    switch(msg_type) {
        case message::RUN: {
            iotmp_message response(request.get_stream_id(), message::type::OK);
            bool success = resource->run_resource(request, response);
            response.set_message_type(success ? message::type::OK : message::type::ERROR);
            send_message(response);

            // If input was received and stream is active, echo current state
            if(request.has_payload() && resource->stream_enabled() && resource->stream_echo() &&
               (resource->get_io_type() == iotmp_resource::input_wrapper ||
                resource->get_io_type() == iotmp_resource::input_output_wrapper)) {
                stream_resource(*resource, resource->get_stream_id());
            }
            break;
        }

        case message::DESCRIBE: {
            iotmp_message response(request.get_stream_id(), message::type::OK);
            resource->describe(response);
            send_message(response);
            break;
        }

        case message::START_STREAM: {
            uint16_t stream_id = request.get_stream_id();
            auto& cfg = streams_[stream_id];
            cfg.resource = resource;

            // Check for interval parameter
            if(request.has_params()) {
                const auto& params = request.params();
                if(params.contains("interval")) {
                    cfg.interval_ms = params["interval"].get<uint32_t>();
                }
            }

            if(cfg.interval_ms == 0) {
                resource->set_stream_id(stream_id);
            }

            iotmp_message response(stream_id, message::type::OK);
            send_message(response);

            if(resource->stream_echo()) {
                stream_resource(*resource, stream_id);
            }
            break;
        }

        case message::STOP_STREAM: {
            uint16_t stream_id = request.get_stream_id();
            if(resource->get_stream_id() == stream_id) {
                resource->set_stream_id(0);
            }
            streams_.erase(stream_id);

            iotmp_message response(stream_id, message::type::OK);
            send_message(response);
            break;
        }

        case message::STREAM_DATA: {
            iotmp_message response(request.get_stream_id(), message::type::STREAM_DATA);
            resource->run_resource(request, response);

            if(resource->stream_echo() &&
               (resource->get_io_type() == iotmp_resource::input_wrapper ||
                resource->get_io_type() == iotmp_resource::input_output_wrapper)) {
                stream_resource(*resource, request.get_stream_id());
            }
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// Streaming
// ============================================================================

bool client::stream_resource(iotmp_resource& resource, uint16_t stream_id) {
    iotmp_message request(message::type::STREAM_DATA);
    iotmp_message response(message::type::STREAM_DATA);
    resource.run_resource(request, response);

    auto& msg = response.has_field(message::field::PAYLOAD) ? response : request;
    if(msg.has_field(message::field::PAYLOAD)) {
        msg.set_stream_id(stream_id);
        send_message(msg);
        return true;
    }
    return false;
}

void client::check_stream_intervals() {
    int64_t now = uptime_ms();
    for(auto& [stream_id, cfg] : streams_) {
        if(cfg.interval_ms > 0 && cfg.resource) {
            if(now - cfg.last_streaming >= cfg.interval_ms) {
                cfg.last_streaming = now;
                stream_resource(*cfg.resource, stream_id);
            }
        }
    }
}

bool client::stream(const char* resource_name) {
    auto it = resources_.find(resource_name);
    if(it == resources_.end() || !it->second.stream_enabled()) return false;
    return stream_resource(it->second, it->second.get_stream_id());
}

// ============================================================================
// Server API
// ============================================================================

bool client::server_request(iotmp_message& msg, json_t* response_payload) {
    if(!connected_) return false;

    msg.set_random_stream_id();
    uint16_t expected_stream_id = msg.get_stream_id();

    if(!enqueue_message(msg)) return false;

    // Wait for response (simple blocking approach)
    iotmp_message response(message::type::RESERVED);
    int attempts = 0;
    while(connected_ && attempts < 100) {
        if(read_message(response)) {
            if(response.get_stream_id() == expected_stream_id &&
               response.get_message_type() <= message::type::ERROR) {
                if(response_payload && response.has_payload()) {
                    response_payload->swap(response.payload());
                }
                return response.get_message_type() == message::type::OK;
            }
            // Not our response -- handle it normally
            handle_message(response);
        }
        attempts++;
    }
    return false;
}

bool client::set_property(const char* property_id, json_t data) {
    iotmp_message msg(message::type::RUN);
    msg[message::field::RESOURCE] = static_cast<uint32_t>(server::run::SET_DEVICE_PROPERTY);
    msg[message::field::PARAMETERS] = std::string(property_id);
    msg[message::field::PAYLOAD].swap(data);
    return server_request(msg);
}

bool client::get_property(const char* property_id, json_t& data) {
    iotmp_message msg(message::type::RUN);
    msg[message::field::RESOURCE] = static_cast<uint32_t>(server::run::READ_DEVICE_PROPERTY);
    msg[message::field::PARAMETERS] = std::string(property_id);
    return server_request(msg, &data);
}

bool client::write_bucket(const char* bucket_id, json_t data) {
    iotmp_message msg(message::type::RUN);
    msg[message::field::RESOURCE] = static_cast<uint32_t>(server::run::WRITE_BUCKET);
    msg[message::field::PARAMETERS] = std::string(bucket_id);
    msg[message::field::PAYLOAD].swap(data);
    return server_request(msg);
}

bool client::call_endpoint(const char* endpoint_name) {
    iotmp_message msg(message::type::RUN);
    msg[message::field::RESOURCE] = static_cast<uint32_t>(server::run::CALL_ENDPOINT);
    msg[message::field::PARAMETERS] = std::string(endpoint_name);
    return server_request(msg);
}

bool client::call_endpoint(const char* endpoint_name, json_t data) {
    iotmp_message msg(message::type::RUN);
    msg[message::field::RESOURCE] = static_cast<uint32_t>(server::run::CALL_ENDPOINT);
    msg[message::field::PARAMETERS] = std::string(endpoint_name);
    msg[message::field::PAYLOAD].swap(data);
    return server_request(msg);
}

// ============================================================================
// Helpers
// ============================================================================

iotmp_resource* client::find_resource(const std::string& path) {
    auto it = resources_.find(path);
    if(it != resources_.end()) return &it->second;
    return nullptr;
}

void client::notify_state(client_state state) {
    state_ = state;
    if(state_callback_) {
        state_callback_(state);
    }
}

} // namespace thinger::iotmp
