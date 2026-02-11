#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_WS2812.h"
#include "sdkconfig.h"

static const char *TAG = "APP";

void app_main(void)
{
    led_ws2812_init(CONFIG_BLINK_GPIO);
    
    ESP_LOGI(TAG, "Starting LED blink with period %d ms", CONFIG_BLINK_PERIOD);

    while (1) {
        ESP_LOGI(TAG, "LED ON");
        led_ws2812_set_color(255, 255, 255); 
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BLINK_PERIOD / 2));

        ESP_LOGI(TAG, "LED OFF");
        led_ws2812_clear();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BLINK_PERIOD / 2));
    }
}