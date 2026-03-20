# IOTMP Client for ESP-IDF

IOTMP protocol client component for [ESP-IDF](https://github.com/espressif/esp-idf) (v5.x/v6.x). Connects ESP32 devices to the [Thinger.io](https://thinger.io) IoT platform using the IOTMP binary protocol.

Built on the shared [IOTMP-Embedded](https://github.com/thinger-io/IOTMP-Embedded) core library (same protocol engine used by the Zephyr and Arduino clients).

## Features

- TLS-secured connections via `esp_tls` (enabled by default)
- Dedicated FreeRTOS task with `select()`-based event loop
- Thread-safe server API (properties, buckets, endpoints) callable from any task
- Automatic reconnection with exponential backoff
- Resource streaming with configurable intervals
- Full Kconfig integration for compile-time configuration

## Quick Start

### 1. Project Structure

```
your_project/
├── main/
│   ├── CMakeLists.txt
│   └── main.cpp
├── CMakeLists.txt
├── components/
│   ├── iotmp-espidf/      (this component)
│   └── iotmp-embedded/    (core protocol library)
```

Or set `EXTRA_COMPONENT_DIRS` in your project `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "/path/to/iotmp-espidf"
    "/path/to/iotmp-embedded"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_thinger_project)
```

### 2. Code

```cpp
#include <thinger/iotmp/client.hpp>

// After WiFi is connected...
static thinger::iotmp::client thing;
thing.set_credentials("username", "device_id", "device_credential");
thing.set_host("iot.thinger.io");

// Define resources
thing["temperature"] = [](thinger::iotmp::output& out) {
    out["celsius"] = 23.5f;
};

thing["relay"] = [](thinger::iotmp::input& in) {
    bool state = in["on"].get<bool>();
    gpio_set_level(RELAY_GPIO, state);
};

// Start (creates a FreeRTOS task)
thing.start();
```

### 3. Build & Flash

```bash
idf.py build
idf.py flash monitor
```

## Kconfig Options

Configure via `idf.py menuconfig` under **Thinger IOTMP Client**:

| Option | Default | Description |
|--------|---------|-------------|
| `THINGER_IOTMP_STACK_SIZE` | 8192 | Client task stack size (bytes) |
| `THINGER_IOTMP_PRIORITY` | 5 | Client task priority |
| `THINGER_IOTMP_KEEPALIVE_SECONDS` | 60 | Keep-alive interval |
| `THINGER_IOTMP_MAX_MESSAGE_SIZE` | 4096 | Maximum IOTMP message size (bytes) |
| `THINGER_IOTMP_TLS` | y | Enable TLS (port 25206) or plain TCP (port 25204) |
| `THINGER_IOTMP_RECONNECT_BASE_MS` | 1000 | Initial reconnection delay |
| `THINGER_IOTMP_RECONNECT_MAX_MS` | 60000 | Maximum reconnection delay |

## Server API

The server API methods are thread-safe and can be called from any FreeRTOS task:

```cpp
// Properties
thing.set_property("config", json_t{{"threshold", 42}});
json_t data;
thing.get_property("config", data);

// Data buckets
thing.write_bucket("sensor_data", json_t{{"temp", 23.5}});

// Endpoints (email, webhooks, etc.)
thing.call_endpoint("alert");
thing.call_endpoint("notify", json_t{{"message", "hello"}});

// Manual streaming
thing.stream("temperature");
```

## Dependencies

- [IOTMP-Embedded](https://github.com/thinger-io/IOTMP-Embedded) — shared protocol core
- ESP-IDF >= 5.0

## Documentation

- [Thinger.io Documentation](https://docs.thinger.io)
- [IOTMP Protocol](https://docs.thinger.io/iotmp)

## License

MIT License - Copyright (c) INTERNET OF THINGER SL
