# 领域核心不依赖 UART、RTOS 或芯片驱动。
set(SHARED_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../shared")
idf_component_register(SRCS "${SHARED_ROOT}/domain/src/nursery_model.c"
    INCLUDE_DIRS "${SHARED_ROOT}/domain/include")
