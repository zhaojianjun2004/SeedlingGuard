# 两端共用的纯 C 编解码，业务服务只调用协议接口。
set(SHARED_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../shared")
idf_component_register(SRCS "${SHARED_ROOT}/protocol/src/gateway_protocol.c"
    INCLUDE_DIRS "${SHARED_ROOT}/protocol/include" REQUIRES nursery_domain)
