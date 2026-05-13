if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(airgradient_wifi_tests CXX)

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

add_library(wifi_test_support
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-wifi/services/wifi_manager.cpp"
)

target_include_directories(wifi_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-wifi"
)

target_compile_definitions(wifi_test_support PUBLIC TEST_HOST)

add_executable(airgradient_wifi_tests
    wifi_types.tests.cpp
    wifi_manager.tests.cpp
)

target_include_directories(airgradient_wifi_tests PRIVATE
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-wifi"
)

target_link_libraries(airgradient_wifi_tests PRIVATE
    wifi_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(airgradient_wifi_tests)
