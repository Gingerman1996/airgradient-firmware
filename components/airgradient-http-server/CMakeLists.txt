idf_component_register(
    SRCS "drivers/idf_http_server.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common esp_http_server
)
