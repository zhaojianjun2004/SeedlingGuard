idf_component_register(
    SRCS "src/update_transport_http.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client update_contracts
)
