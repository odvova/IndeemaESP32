idf_component_register(SRCS "my_device.c"
                       INCLUDE_DIRS "."
                       REQUIRES "wifi_component" "http_config" "joystick")