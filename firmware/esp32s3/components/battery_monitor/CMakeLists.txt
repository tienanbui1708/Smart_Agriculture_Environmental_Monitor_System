idf_component_register(
    SRCS "battery_monitor.c" "battery_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_adc
)
