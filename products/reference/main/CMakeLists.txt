idf_component_register(
    SRCS "main.cpp"
         "reference_settings.cpp"
         "test_ble.cpp"
         "test_config.cpp"
         "test_gpio.cpp"
         "test_http_server.cpp"
         "test_nand_storage.cpp"
         "test_payload_cache.cpp"
         "test_sensors.cpp"
         "test_serial.cpp"
         "test_touch.cpp"
         "test_wifi.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-ble
             airgradient-bms
             airgradient-common
             airgradient-config
             airgradient-gpio
             airgradient-http-server
             airgradient-nand-storage
             airgradient-payload-cache
             airgradient-sensors
             airgradient-serial
             airgradient-touch
             airgradient-wifi
             esp_driver_gpio
             esp_driver_i2c
             esp_driver_spi
             esp_event
             esp_http_server
             esp_netif
             esp_wifi
             nvs_flash
    EMBED_FILES "web/index.html"
)
