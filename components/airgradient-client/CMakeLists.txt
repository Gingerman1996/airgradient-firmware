set(srcs
    "services/ag_client.cpp"
    "services/payload_serializer.cpp"
    "backends/wifi_http_client.cpp"

    # CoAP packet helper (vendored as-is from the old airgradient-client
    # library).  Compiled now so the future cellular CoAP backend can link
    # against it without further build-system churn.
    "lib/coap-packet/src/CoapBuilder.cpp"
    "lib/coap-packet/src/CoapParser.cpp"

    # Binary payload encoder (vendored as-is).
    "lib/payload-encoder/src/PayloadEncoder.cpp"
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS
        "."
        "lib/coap-packet/src"
        "lib/payload-encoder/src"
    REQUIRES
        airgradient-common
        esp_http_client
        json
)
