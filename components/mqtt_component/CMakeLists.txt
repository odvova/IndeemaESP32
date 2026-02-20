idf_component_register(
    SRCS "mqtt_component.c"
    INCLUDE_DIRS "."
    REQUIRES mqtt wifi_component led_WS2812
    PRIV_REQUIRES cJSON
)
