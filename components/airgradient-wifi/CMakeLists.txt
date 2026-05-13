idf_component_register(
    SRCS "services/wifi_manager.cpp"
         "drivers/esp_wifi_hal.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip espressif__mdns
)
