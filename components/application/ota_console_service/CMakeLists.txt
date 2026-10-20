idf_component_register(
    SRCS "src/ota_console_service.c"
    INCLUDE_DIRS "include"
    REQUIRES console firmware_update_service gateway_link esp_timer wifi_manager
)
