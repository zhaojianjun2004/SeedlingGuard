idf_component_register(SRCS "src/gateway_link.c" INCLUDE_DIRS "include"
    REQUIRES gateway_protocol nursery_domain uart_port esp_timer freertos)
