#ifndef LED_WS2812_H
#define LED_WS2812_H

#include "led_strip.h"
#include "esp_err.h"

/**
 * @brief Initialize the LED strip
 * 
 * @param gpio_num GPIO pin number for the LED strip
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_ws2812_init(int gpio_num);

/**
 * @brief Set LED color
 * 
 * @param red Red value (0-255)
 * @param green Green value (0-255)
 * @param blue Blue value (0-255)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_ws2812_set_color(uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Turn off the LED
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_ws2812_clear(void);

/**
 * @brief Deinitialize the LED strip
 */
void led_ws2812_deinit(void);

#endif // LED_WS2812_H
