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

#ifndef THINGER_IOTMP_CLIENT_HPP
#define THINGER_IOTMP_CLIENT_HPP

// Map IOTMP logging to ESP-IDF logging — must be defined before core headers
#include "esp_log.h"

#ifndef THINGER_LOG_ERROR
#define THINGER_LOG_ERROR(fmt, ...)   ESP_LOGE("iotmp", fmt, ##__VA_ARGS__)
#endif
#ifndef THINGER_LOG_WARNING
#define THINGER_LOG_WARNING(fmt, ...) ESP_LOGW("iotmp", fmt, ##__VA_ARGS__)
#endif
#ifndef THINGER_LOG_INFO
#define THINGER_LOG_INFO(fmt, ...)    ESP_LOGI("iotmp", fmt, ##__VA_ARGS__)
#endif
#ifndef THINGER_LOG_DEBUG
#define THINGER_LOG_DEBUG(fmt, ...)   ESP_LOGD("iotmp", fmt, ##__VA_ARGS__)
#endif

// Override platform macros before core includes them as no-ops
#include "driver/gpio.h"

#undef digitalPin
#define digitalPin(PIN) [](thinger::iotmp::input& in) { \
    static bool state = false;                           \
    if(in.is_empty()) {                                  \
        in = state;                                      \
    } else {                                             \
        state = (bool)in;                                \
        gpio_set_level((gpio_num_t)(PIN), state ? 1 : 0);\
    }                                                    \
}

#undef analogPin
#define analogPin(PIN) [](thinger::iotmp::output& out) { out = (int)gpio_get_level((gpio_num_t)(PIN)); }

#include <thinger/iotmp/iotmp.hpp>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"

#ifdef CONFIG_THINGER_IOTMP_TLS
#include "esp_tls.h"
#endif

#include <string>
#include <queue>
#include <functional>

namespace thinger::iotmp {

    class client : public iotmp_client_base<client> {
    public:
        client();
        ~client();

        // Lifecycle
        esp_err_t start();
        void stop();

        // State
        bool is_connected() const { return connected_; }
        client_state get_state() const { return state_; }

        // Server API (thread-safe — call from any task)
        bool set_property(const char* property_id, json_t data);
        bool get_property(const char* property_id, json_t& data);
        bool write_bucket(const char* bucket_id, json_t data);
        bool call_endpoint(const char* endpoint_name);
        bool call_endpoint(const char* endpoint_name, json_t data);

        // ----- CRTP transport implementation -------------------------

        bool send_bytes_impl(const void* data, size_t len);
        bool recv_bytes_impl(void* buf, size_t len);
        bool is_connected_impl() const;
        unsigned long get_millis() const;
        void on_disconnect();

        // Send message from any task (queues + wakes select)
        bool enqueue_message(iotmp_message& msg);

    private:
        // Task entry point
        static void task_entry(void* arg);

        // Main loop (runs in client task)
        void run();

        // Connection lifecycle
        int do_connect();
        void do_disconnect();

        // Flush TX queue (called from client task)
        void flush_tx_queue();

        // Server API helper (sends RUN message and waits for response)
        bool server_request(iotmp_message& msg, json_t* response_payload = nullptr);

        // State management (also notifies via base class callback)
        void set_state(client_state state);

        // Socket (plain TCP)
        int sock_ = -1;

#ifdef CONFIG_THINGER_IOTMP_TLS
        // TLS handle
        esp_tls_t* tls_ = nullptr;
#endif

        // Task
        TaskHandle_t task_handle_ = nullptr;
        volatile bool running_ = false;
        volatile bool connected_ = false;
        client_state state_ = client_state::DISCONNECTED;

        // TX queue (for cross-task message sending)
        SemaphoreHandle_t tx_mutex_ = nullptr;
        std::queue<std::string> tx_queue_;

        // Read buffer
        uint8_t read_buffer_[CONFIG_THINGER_IOTMP_MAX_MESSAGE_SIZE];
    };

}

#endif
