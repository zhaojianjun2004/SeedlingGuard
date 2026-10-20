idf_component_register(
    SRCS "src/mqtt_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_hw_support esp_timer freertos gateway_outbox gateway_link json mqtt
)
