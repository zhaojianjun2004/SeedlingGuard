idf_component_register(
    SRCS "update_integrity_sha256.c"
    INCLUDE_DIRS "include"
    REQUIRES firmware_update_service mbedtls
)
