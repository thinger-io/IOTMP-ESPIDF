idf_component_register(
    SRCS "src/iotmp_client.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash lwip iotmp-embedded esp_driver_gpio
    PRIV_REQUIRES esp-tls esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
