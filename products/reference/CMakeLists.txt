cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components"
)

# Explicitly list components this product needs.
# airgradient-ble and esp-nimble-cpp require CONFIG_BT_ENABLED=y and
# CONFIG_BT_NIMBLE_ENABLED=y to compile (set in sdkconfig.defaults when
# RUN_TEST_BLE is selected). They are included here because the reference
# product is a test bed that exercises all available hardware.
set(COMPONENTS
    main
    airgradient-ble
    airgradient-common
    airgradient-config
    airgradient-gpio
    airgradient-http-server
    airgradient-nand-storage
    airgradient-payload-cache
    airgradient-sensors
    airgradient-serial
    airgradient-touch
    ads1115
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(airgradient-reference)
