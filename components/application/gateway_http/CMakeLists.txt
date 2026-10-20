idf_component_register(
    SRCS "src/gateway_http.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_hw_support esp_timer gateway_link gateway_outbox json mqtt_service wifi_manager
    EMBED_FILES "index.html"
)
