idf_component_register(
    SRCS "services/provisioning_manager.cpp"
         "internal/scan_filter.cpp"
         "internal/dns_packet.cpp"
         "internal/captive_dns_responder.cpp"
         "internal/provisioning_timer.cpp"
         "internal/wifi_portal_transport.cpp"
         "internal/provisioning_json.cpp"
         "internal/ble_transport.cpp"
         "internal/provisioning_qr.cpp"
         "vendor/qrcodegen.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "vendor"
    REQUIRES airgradient-common
             airgradient-ble
             airgradient-http-server
             airgradient-wifi
             json
    PRIV_REQUIRES esp_timer lwip
    EMBED_FILES "assets/portal.html"
)
