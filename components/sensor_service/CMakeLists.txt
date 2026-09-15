idf_component_register(
    SRCS "sensor_service.c"
    INCLUDE_DIRS "include"
    REQUIRES aht20 driver freertos esp_timer
)
