idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES app_config_service ble_service config_store mqtt_service nvs_flash sensor_service wifi_manager
)
