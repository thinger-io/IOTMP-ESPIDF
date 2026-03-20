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
    return tls_ != nullptr;
#else
    return sock_ >= 0;
#endif
}

unsigned long client::get_millis() const {
    return static_cast<unsigned long>(esp_timer_get_time() / 1000);
}

// ============================================================================
// CRTP connection implementation
// ============================================================================

bool client::connect_impl() {
#ifdef CONFIG_THINGER_IOTMP_TLS
    // Use esp_tls for TLS connections
    esp_tls_cfg_t cfg = {};
    cfg.timeout_ms = 10000;
    cfg.non_block = false;
    // Use system certificate bundle if available
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    tls_ = esp_tls_init();
    if(!tls_) {
        ESP_LOGE(TAG, "Failed to allocate esp_tls handle");
        return false;
    }

    int ret = esp_tls_conn_new_sync(host_, strlen(host_), port_, &cfg, tls_);
    if(ret != 1) {
        ESP_LOGE(TAG, "TLS connection failed to %s:%d", host_, port_);
        esp_tls_conn_destroy(tls_);
        tls_ = nullptr;
        return false;
    }
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
        return false;
    }

    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if(sock_ < 0) {
        ESP_LOGE(TAG, "Socket creation failed: %d", errno);
        freeaddrinfo(res);
        return false;
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
        return false;
    }
#endif

    return true;
}

void client::disconnect_impl() {
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
}

int client::get_socket_fd() const {
#ifdef CONFIG_THINGER_IOTMP_TLS
    if(!tls_) return -1;
    int fd = -1;
    esp_tls_get_conn_sockfd(tls_, &fd);
    return fd;
#else
    return sock_;
#endif
}

bool client::data_available_impl() {
    int fd = get_socket_fd();
    if(fd < 0) return false;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 }; // 100ms
    int rc = select(fd + 1, &read_fds, nullptr, nullptr, &tv);

    // Also flush TX queue on each poll cycle
    flush_tx_queue();

#ifdef CONFIG_THINGER_IOTMP_TLS
    // TLS may have buffered data even if select() says no
    if(tls_ && esp_tls_get_bytes_avail(tls_) > 0) {
        return true;
    }
#endif

    return rc > 0 && FD_ISSET(fd, &read_fds);
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
        int timeout = 100; // 10 seconds (100 * 100ms)
        while(eTaskGetState(task_handle_) != eDeleted && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout--;
        }
        task_handle_ = nullptr;
    }

    disconnect();

    ESP_LOGI(TAG, "IOTMP client stopped");
}

// ============================================================================
// Task
// ============================================================================

void client::task_entry(void* arg) {
    auto* self = static_cast<client*>(arg);
    self->run();
    vTaskDelete(nullptr);
}

void client::run() {
    while(running_) {
        this->handle();
    }
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

} // namespace thinger::iotmp
