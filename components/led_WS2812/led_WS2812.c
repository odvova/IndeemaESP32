#include <stdio.h>
#include "led_WS2812.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "LED_WS2812";
static led_strip_handle_t led_strip = NULL;

esp_err_t led_ws2812_init(int gpio_num)
{
    if (led_strip != NULL) {
        ESP_LOGW(TAG, "LED strip already initialized");
        return ESP_OK;
    }

    // LED strip general settings
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio_num,
        .max_leds = 1,  // Single LED
    };

#ifdef CONFIG_BLINK_LED_STRIP_BACKEND_RMT
    // RMT backend configuration
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,  // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
#elif CONFIG_BLINK_LED_STRIP_BACKEND_SPI
    // SPI backend configuration
    led_strip_spi_config_t spi_config = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &led_strip));
#else
    #error "Please select LED strip backend in menuconfig"
#endif

    // All strip types support this
    ESP_ERROR_CHECK(led_strip_clear(led_strip));
    
    ESP_LOGI(TAG, "LED strip initialized on GPIO %d", gpio_num);
    return ESP_OK;
}

esp_err_t led_ws2812_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (led_strip == NULL) {
        ESP_LOGE(TAG, "LED strip not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, red, green, blue));
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));
    
    return ESP_OK;
}

esp_err_t led_ws2812_clear(void)
{
    if (led_strip == NULL) {
        ESP_LOGE(TAG, "LED strip not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_ERROR_CHECK(led_strip_clear(led_strip));
    return ESP_OK;
}

void led_ws2812_deinit(void)
{
    if (led_strip != NULL) {
        led_strip_del(led_strip);
        led_strip = NULL;
        ESP_LOGI(TAG, "LED strip deinitialized");
    }
}
