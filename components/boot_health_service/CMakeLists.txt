idf_component_register(
    SRCS "boot_health_service.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_app_format esp_system freertos
)
