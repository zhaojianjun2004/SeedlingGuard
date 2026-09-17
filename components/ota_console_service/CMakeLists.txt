idf_component_register(
    SRCS "ota_console_service.c"
    INCLUDE_DIRS "include"
    REQUIRES console firmware_update_service
)
