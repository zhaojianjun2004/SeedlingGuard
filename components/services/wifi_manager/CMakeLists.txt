idf_component_register(
    SRCS "src/wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
)
