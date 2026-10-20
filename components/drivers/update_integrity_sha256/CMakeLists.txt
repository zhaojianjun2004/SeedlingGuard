idf_component_register(
    SRCS "src/update_integrity_sha256.c"
    INCLUDE_DIRS "include"
    REQUIRES update_contracts mbedtls
)
