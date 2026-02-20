idf_component_register(SRCS "wifi_component.c"
                    INCLUDE_DIRS "include"
                    REQUIRES "freertos" "esp_wifi" "esp_event" "esp_netif" "nvs_flash" "lwip" "led_WS2812")
