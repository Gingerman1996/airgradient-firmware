if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(airgradient_http_server_tests CXX)

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
    FetchContent_MakeAvailable(Catch2)

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(CTest)
    include(Catch)
endif()

# Header-only test support library. The component's runtime code is
# ESP-IDF-bound (drivers/idf_http_server.cpp depends on esp_http_server),
# so host tests exercise the hal/types layer plus the TestHttpRequest
# helper directly and do not link the driver.
add_library(http_server_test_support INTERFACE)

target_include_directories(http_server_test_support INTERFACE
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-http-server"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-http-server/tests"
)

target_compile_definitions(http_server_test_support INTERFACE TEST_HOST)

add_executable(http_server_tests
    http_handler.tests.cpp
)

target_link_libraries(http_server_tests PRIVATE
    http_server_test_support
    Catch2::Catch2WithMain
)

catch_discover_tests(http_server_tests)
