idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES app_config_service ble_service boot_health_service config_store firmware_update_service mqtt_service nvs_flash ota_button_service ota_console_service sensor_service update_integrity_sha256 update_manifest_json update_platform_esp32 update_transport_http wifi_manager
)
