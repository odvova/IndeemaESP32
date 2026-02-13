idf_component_register(SRCS "joystick.c"
                    INCLUDE_DIRS "include"
                    REQUIRES "freertos" "driver" esp_hal_ana_conv)
