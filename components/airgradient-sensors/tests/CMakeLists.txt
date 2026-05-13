if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(airgradient_sensors_tests C CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    get_filename_component(AIRGRADIENT_REPO_ROOT
                           "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
                           ABSOLUTE)

    include(FetchContent)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_Declare(
        trompeloeil
        GIT_REPOSITORY https://github.com/rollbear/trompeloeil.git
        GIT_TAG        v47
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2 trompeloeil)

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(CTest)
    include(Catch)
endif()

add_library(sensors_test_support
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-sensors/services/sensor_manager.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/rtos.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/sensirion-gas-index-algorithm/sensirion_gas_index_algorithm.c"
)

target_include_directories(sensors_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-sensors"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
    "${AIRGRADIENT_REPO_ROOT}/components/sensirion-gas-index-algorithm"
)

target_compile_definitions(sensors_test_support PUBLIC TEST_HOST)

add_executable(sensors_tests
    sensor_manager.tests.cpp
)

target_link_libraries(sensors_tests PRIVATE
    sensors_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(sensors_tests)
