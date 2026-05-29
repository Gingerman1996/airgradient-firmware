idf_component_register(
    SRCS "main.cpp"
         "go_app.cpp"
         "go_hardware_board.cpp"
         "go_ble.cpp"
         "go_buzzer.cpp"
         "go_melody.cpp"
         "go_types.cpp"
         "go_settings.cpp"
         "go_display.cpp"
         "gps/gps_driver.cpp"
         "gps/gps_service.cpp"
         "go_accel.cpp"
         "go_input.cpp"
         "go_io_expander.cpp"
         "go_led.cpp"
         "go_orchestrator.cpp"
         "blearn_controller.cpp"
         "go_power.cpp"
         "go_sensor_producer.cpp"
         "go_storage.cpp"
         "go_ui.cpp"
         "go_ulp.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-bms
             bq25629
    REQUIRES airgradient-ble
             airgradient-common
             airgradient-config
              airgradient-gpio
              airgradient-nand-storage
              airgradient-serial
              libnmea-esp32
             airgradient-payload-cache
             airgradient-sensors
             airgradient-touch
             esp_driver_gpio
             esp_driver_i2c
             esp_driver_ledc
             esp_driver_spi
             nvs_flash
             u8g2
             fatfs
             ulp
)

ulp_embed_binary(ulp_wdt "ulp/wdt_feed.c" "")

# Enable verbose GPS debug logging by exporting AG_DEBUG_GPS before the build:
#   AG_DEBUG_GPS=1 idf.py -C products/go build
# Default builds stay quiet.
if(DEFINED ENV{AG_DEBUG_GPS})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AG_DEBUG_GPS=1)
    message(STATUS "AGo: AG_DEBUG_GPS=1 — GpsDriver=VERBOSE, GpsService=DEBUG")
endif()
