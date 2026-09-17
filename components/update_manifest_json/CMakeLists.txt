idf_component_register(
    SRCS "update_manifest_json.c"
    INCLUDE_DIRS "include"
    REQUIRES firmware_update_service json
)
