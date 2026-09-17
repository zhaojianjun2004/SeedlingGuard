idf_component_register(
    SRCS "update_platform_esp32.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update bootloader_support esp_app_format firmware_update_service
)
