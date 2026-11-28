idf_component_register(
    SRCS "storage_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash record_log esp_partition esp_hw_support log
)
