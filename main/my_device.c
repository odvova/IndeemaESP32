#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_WS2812.h"
#include "joystick.h"
#include "button.h"
#include "sdkconfig.h"

static const char *TAG = "APP";

#define BLINK_PERIOD_MS 400

typedef enum {
    LED_MODE_BLINK = 0,
    LED_MODE_JOYSTICK,
} led_mode_t;

static volatile led_mode_t s_mode = LED_MODE_BLINK;

static void on_button_press(void *user_data)
{
    (void)user_data;
    ESP_LOGI(TAG, "event=PRESS");
}

static void on_button_long_press(void *user_data)
{
    (void)user_data;
    ESP_LOGI(TAG, "event=LONG_PRESS");
}

static void on_button_click(void *user_data)
{
    (void)user_data;
    s_mode = LED_MODE_JOYSTICK;
    ESP_LOGI(TAG, "event=CLICK -> mode=JOYSTICK");
}

static void on_button_double_click(void *user_data)
{
    (void)user_data;
    s_mode = LED_MODE_BLINK;
    ESP_LOGI(TAG, "event=DOUBLE_CLICK -> mode=BLINK");
}

void app_main(void)
{
    bool blink_on = false;
    TickType_t last_blink_tick = xTaskGetTickCount();
    TickType_t last_log = xTaskGetTickCount();
    joystick_sample_t sample = {0};
    joystick_led_state_t led_state = {0};
    joystick_runtime_t joystick_runtime = {0};
    int joystick_button_gpio = joystick_get_switch_gpio();
    button_handle_t joystick_button = NULL;
    const button_config_t button_config = BUTTON_COMPONENT_CONFIG_DEFAULT(joystick_button_gpio, 0);
    const button_callbacks_t button_callbacks = {
        .on_press = on_button_press,
        .on_long_press = on_button_long_press,
        .on_click = on_button_click,
        .on_double_click = on_button_double_click,
        .user_data = NULL,
    };

    led_ws2812_init(CONFIG_BLINK_GPIO);
    ESP_ERROR_CHECK(joystick_init());
    joystick_runtime_reset(&joystick_runtime);
    ESP_ERROR_CHECK(button_component_create(&button_config, &button_callbacks, &joystick_button));

    while (1) {
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        TickType_t now = xTaskGetTickCount();

        (void)joystick_read_sample(&sample);

        if (s_mode == LED_MODE_BLINK) {
            if ((now - last_blink_tick) >= pdMS_TO_TICKS(BLINK_PERIOD_MS)) {
                blink_on = !blink_on;
                last_blink_tick = now;
            }

            if (blink_on) {
                led_ws2812_set_color(32, 32, 32);
            } else {
                led_ws2812_clear();
            }

            if ((now - last_log) >= pdMS_TO_TICKS(1000)) {
                ESP_LOGI(TAG, "mode=BLINK sw=%d", (int)sample.sw_pressed);
                last_log = now;
            }

            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        joystick_map_sample_to_led(&joystick_runtime, &sample, &led_state);
        red = led_state.red;
        green = led_state.green;
        blue = led_state.blue;

        if (!led_state.led_on) {
            led_ws2812_clear();
        } else {
            led_ws2812_set_color(red, green, blue);
        }

        if ((now - last_log) >= pdMS_TO_TICKS(500)) {
            ESP_LOGI(TAG, "mode=JOYSTICK raw=(%d,%d) norm=(%d,%d) span=(%d,%d) sw=%d -> rgb=(%u,%u,%u) br=%u",
                     led_state.x_raw, led_state.y_raw, led_state.x_norm, led_state.y_norm, led_state.x_span, led_state.y_span,
                     (int)led_state.sw_pressed, red, green, blue, led_state.brightness);
            last_log = now;
        }

        vTaskDelay(pdMS_TO_TICKS(60));
    }
}