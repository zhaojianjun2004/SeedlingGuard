idf_component_register(
    SRCS "update_transport_http.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client firmware_update_service
)
