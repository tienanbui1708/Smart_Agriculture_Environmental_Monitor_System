idf_component_register(
    SRCS "status_led.c" "status_led_policy.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer log led_strip
)
