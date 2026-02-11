idf_component_register(
    SRCS "led_WS2812.c"
    INCLUDE_DIRS "include"
    REQUIRES led_strip driver
)
