idf_component_register(
    SRCS "src/update_platform_esp32.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update bootloader_support esp_app_format update_contracts
)
