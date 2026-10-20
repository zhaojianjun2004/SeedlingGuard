# 手写层的构建边界：领域/协议不能取得 HAL 或服务私有头文件。
set(NURSERY_SHARED "${CMAKE_SOURCE_DIR}/../../shared")
add_library(nursery_domain STATIC "${NURSERY_SHARED}/domain/src/nursery_model.c")
target_include_directories(nursery_domain PUBLIC "${NURSERY_SHARED}/domain/include")

add_library(nursery_protocol STATIC
    "${NURSERY_SHARED}/protocol/src/gateway_protocol.c"
    Protocol/Src/modbus_rtu.c Protocol/Src/can_diag_protocol.c)
target_include_directories(nursery_protocol PUBLIC Protocol/Inc "${NURSERY_SHARED}/protocol/include")
target_link_libraries(nursery_protocol PUBLIC nursery_domain)

add_library(board_support STATIC Bsp/Src/board_system.c Bsp/Src/board_flash_port.c
    Bsp/Src/rs485_port.c Bsp/Src/rs485_timing.c Bsp/Src/field_link_port.c)
target_include_directories(board_support PUBLIC Bsp/Inc)
target_link_libraries(board_support PUBLIC stm32cubemx)

add_library(device_drivers STATIC Drivers/Device/Src/w25q_flash.c)
target_include_directories(device_drivers PUBLIC Drivers/Device/Inc)
target_link_libraries(device_drivers PUBLIC board_support)

add_library(field_services STATIC Services/Src/field_state_service.c Services/Src/acquisition_service.c
    Services/Src/field_link_service.c Services/Src/storage_service.c Services/Src/supervisor_service.c)
target_include_directories(field_services PUBLIC Services/Inc PRIVATE Services/Private Config)
target_link_libraries(field_services PUBLIC nursery_domain nursery_protocol device_drivers board_support)

add_library(field_application STATIC Application/Src/gateway_app.c)
target_include_directories(field_application PUBLIC Application/Inc)
target_link_libraries(field_application PRIVATE field_services)
