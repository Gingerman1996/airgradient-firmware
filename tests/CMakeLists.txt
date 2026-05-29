cmake_minimum_required(VERSION 3.16)
project(my_project_tests CXX C)

# Pin static linkage before any subdirectory or FetchContent dep runs.  A
# fetched dep's option(BUILD_SHARED_LIBS ... ON) would otherwise flip this
# cache entry globally and turn other test_support libraries into shared
# objects, breaking links against PIC-less static deps.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static libs in host tests" FORCE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(AIRGRADIENT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Fetch Catch2 and Trompeloeil
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

# Enable testing and load Catch discovery helpers
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

add_subdirectory("${AIRGRADIENT_REPO_ROOT}/components/airgradient-bms/tests"
                 components_airgradient_bms_tests)
add_subdirectory("${AIRGRADIENT_REPO_ROOT}/components/airgradient-sensors/tests"
                 components_airgradient_sensors_tests)
add_subdirectory("${AIRGRADIENT_REPO_ROOT}/components/airgradient-payload-cache/tests"
                 components_airgradient_payload_cache_tests)
add_subdirectory("${AIRGRADIENT_REPO_ROOT}/components/airgradient-cellular/tests"
                 components_airgradient_cellular_tests)
add_subdirectory("${AIRGRADIENT_REPO_ROOT}/components/airgradient-client/tests"
                 components_airgradient_client_tests)
add_subdirectory("${AIRGRADIENT_REPO_ROOT}/components/airgradient-wifi/tests"
                 components_airgradient_wifi_tests)
add_subdirectory("${AIRGRADIENT_REPO_ROOT}/components/airgradient-http-server/tests"
                 components_airgradient_http_server_tests)
add_subdirectory("${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/tests"
                 components_airgradient_provisioning_tests)
add_subdirectory("${AIRGRADIENT_REPO_ROOT}/products/go/tests"
                 products_go_tests)
