idf_component_register(
    SRCS "src/firmware_update_service.c"
    INCLUDE_DIRS "include"
    REQUIRES update_contracts freertos
)
