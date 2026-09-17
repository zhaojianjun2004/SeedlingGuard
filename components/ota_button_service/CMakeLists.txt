idf_component_register(
    SRCS "ota_button_service.c"
    INCLUDE_DIRS "include"
    REQUIRES driver firmware_update_service freertos
)
