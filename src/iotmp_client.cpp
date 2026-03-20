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
// Construction / Configuration
// ============================================================================

client::client() {
    tx_mutex_ = xSemaphoreCreateMutex();
    // Default port depends on TLS config
#ifdef CONFIG_THINGER_IOTMP_TLS
    port_ = 25206;
#else
    port_ = 25204;
#endif
}

client::~client() {
    stop();
    if(tx_mutex_) {
        vSemaphoreDelete(tx_mutex_);
        tx_mutex_ = nullptr;
    }
}

// ============================================================================
// CRTP transport implementation
// ============================================================================

bool client::send_bytes_impl(const void* buf, size_t len) {
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

bool client::recv_bytes_impl(void* buf, size_t len) {
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

bool client::is_connected_impl() const {
#ifdef CONFIG_THINGER_IOTMP_TLS
    return tls_ != nullptr && connected_;
#else
    return sock_ >= 0 && connected_;
#endif
}

unsigned long client::get_millis() const {
    return static_cast<unsigned long>(esp_timer_get_time() / 1000);
}

void client::on_disconnect() {
    connected_ = false;
    notify_state(client_state::DISCONNECTED);
    do_disconnect();
    clear_streams();
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

        // Authenticate (using base class)
        notify_state(client_state::AUTHENTICATING);
        if(!this->authenticate()) {
            ESP_LOGE(TAG, "Authentication failed");
            notify_state(client_state::AUTH_FAILED);
            do_disconnect();
            goto reconnect;
        }

        ESP_LOGI(TAG, "Authenticated as %s@%s", get_device_id(), get_username());
        connected_ = true;
        notify_state(client_state::AUTHENTICATED);
        backoff_ms = CONFIG_THINGER_IOTMP_RECONNECT_BASE_MS;
        notify_state(client_state::READY);

        // Event loop
        {
            int64_t last_activity = get_millis();

            while(running_ && connected_) {
                int64_t elapsed = get_millis() - last_activity;
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
                    if(this->read_message(msg)) {
                        this->handle_message(msg);
                        last_activity = get_millis();
                    } else {
                        ESP_LOGW(TAG, "Failed to read message, disconnecting");
                        break;
                    }
                }

                // Always check TX queue (replaces wakeup pipe)
                if(!running_) break;
                flush_tx_queue();

                // Keepalive timeout
                elapsed = get_millis() - last_activity;
                if(elapsed >= keepalive_ms) {
                    this->send_keepalive();
                    last_activity = get_millis();
                }

                // Check stream intervals (using base class)
                this->check_streams();
            }
        }

        connected_ = false;
        notify_state(client_state::DISCONNECTED);
        do_disconnect();
        this->clear_streams();

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
    ESP_LOGI(TAG, "Connecting to %s:%d...", host_, port_);

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

    int ret = esp_tls_conn_new_sync(host_, strlen(host_), port_, &cfg, tls_);
    if(ret != 1) {
        ESP_LOGE(TAG, "TLS connection failed to %s:%d", host_, port_);
        esp_tls_conn_destroy(tls_);
        tls_ = nullptr;
        return -1;
    }

    ESP_LOGI(TAG, "Connected to %s:%d (TLS)", host_, port_);
#else
    // Plain TCP using BSD sockets
    struct addrinfo hints = {};
    struct addrinfo* res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port_);

    int rc = getaddrinfo(host_, port_str, &hints, &res);
    if(rc != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolve failed for %s: %d", host_, rc);
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

    ESP_LOGI(TAG, "Connected to %s:%d (TCP)", host_, port_);
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
// TX queue (for cross-task message sending)
// ============================================================================

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
        send_bytes(data.data(), data.size());
        tx_queue_.pop();
    }
    xSemaphoreGive(tx_mutex_);
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
        if(this->read_message(response)) {
            if(response.get_stream_id() == expected_stream_id &&
               response.get_message_type() <= message::type::ERROR) {
                if(response_payload && response.has_payload()) {
                    response_payload->swap(response.payload());
                }
                return response.get_message_type() == message::type::OK;
            }
            // Not our response -- handle it normally
            this->handle_message(response);
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

void client::notify_state(client_state state) {
    state_ = state;
    if(state_callback_) {
        state_callback_(state);
    }
}

} // namespace thinger::iotmp
