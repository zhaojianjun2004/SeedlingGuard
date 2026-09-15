idf_component_register(
    SRCS "mqtt_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_hw_support esp_timer freertos json mqtt
)
