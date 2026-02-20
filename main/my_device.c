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

#define ADC_MAX_VALUE 4095
#define ZONE_LOW_TO_MID 1365
#define ZONE_MID_TO_HIGH 2730
#define ZONE_HYSTERESIS 120
#define MIN_EFFECTIVE_SPAN 20
#define BLINK_PERIOD_MS 400
#define JOYSTICK_BUTTON_GPIO 6

typedef enum {
    LED_MODE_BLINK = 0,
    LED_MODE_JOYSTICK,
} led_mode_t;

static volatile led_mode_t s_mode = LED_MODE_BLINK;

typedef struct {
    int min_seen;
    int max_seen;
    int filtered;
    bool initialized;
} axis_state_t;

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int zone_from_norm(int normalized, int previous_zone)
{
    if (previous_zone <= 0) {
        if (normalized < ZONE_LOW_TO_MID) {
            return 0;
        }
        if (normalized < ZONE_MID_TO_HIGH) {
            return 1;
        }
        return 2;
    }

    if (previous_zone == 0) {
        if (normalized > (ZONE_LOW_TO_MID + ZONE_HYSTERESIS)) {
            return 1;
        }
        return 0;
    }

    if (previous_zone == 1) {
        if (normalized < (ZONE_LOW_TO_MID - ZONE_HYSTERESIS)) {
            return 0;
        }
        if (normalized > (ZONE_MID_TO_HIGH + ZONE_HYSTERESIS)) {
            return 2;
        }
        return 1;
    }

    if (normalized < (ZONE_MID_TO_HIGH - ZONE_HYSTERESIS)) {
        return 1;
    }
    return 2;
}

static int normalize_axis(axis_state_t *state, int sample)
{
    if (!state->initialized) {
        state->initialized = true;
        state->filtered = sample;
        state->min_seen = sample;
        state->max_seen = sample;
    } else {
        state->filtered = (state->filtered * 7 + sample) / 8;
        if (state->filtered < state->min_seen) {
            state->min_seen = state->filtered;
        }
        if (state->filtered > state->max_seen) {
            state->max_seen = state->filtered;
        }
    }

    int span = state->max_seen - state->min_seen;
    if (span < MIN_EFFECTIVE_SPAN) {
        return ADC_MAX_VALUE / 2;
    }

    int normalized = (state->filtered - state->min_seen) * ADC_MAX_VALUE / span;
    return clamp_int(normalized, 0, ADC_MAX_VALUE);
}

static int axis_span(const axis_state_t *state)
{
    if (!state->initialized) {
        return 0;
    }
    return state->max_seen - state->min_seen;
}

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
    int x_raw = 0;
    int y_raw = 0;
    int x_norm = 0;
    int y_norm = 0;
    int color_zone = 1;
    int brightness_zone = 1;
    bool sw_pressed = false;
    bool blink_on = false;
    TickType_t last_blink_tick = xTaskGetTickCount();
    TickType_t last_log = xTaskGetTickCount();
    axis_state_t x_state = {0};
    axis_state_t y_state = {0};
    const button_callbacks_t button_callbacks = {
        .on_press = on_button_press,
        .on_long_press = on_button_long_press,
        .on_click = on_button_click,
        .on_double_click = on_button_double_click,
        .user_data = NULL,
    };

    led_ws2812_init(CONFIG_BLINK_GPIO);
    configure_joystick();
    ESP_ERROR_CHECK(button_component_init(JOYSTICK_BUTTON_GPIO, 0, &button_callbacks));

    while (1) {
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        uint8_t brightness = 0;
        TickType_t now = xTaskGetTickCount();

        read_joystick(&x_raw, &y_raw, &sw_pressed);

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
                ESP_LOGI(TAG, "mode=BLINK sw=%d", (int)sw_pressed);
                last_log = now;
            }

            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        x_norm = normalize_axis(&x_state, x_raw);
        y_norm = normalize_axis(&y_state, y_raw);

        color_zone = zone_from_norm(x_norm, color_zone);
        if (color_zone == 0) {
            red = 255;
        } else if (color_zone == 1) {
            green = 255;
        } else {
            blue = 255;
        }

        if (axis_span(&y_state) < MIN_EFFECTIVE_SPAN) {
            brightness_zone = 1;
        } else {
            brightness_zone = zone_from_norm(y_norm, brightness_zone);
        }
        if (brightness_zone == 0) {
            brightness = 0;
        } else if (brightness_zone == 1) {
            brightness = 128;
        } else {
            brightness = 255;
        }

        red = (uint8_t)((red * brightness) / 255);
        green = (uint8_t)((green * brightness) / 255);
        blue = (uint8_t)((blue * brightness) / 255);

        if (brightness == 0) {
            led_ws2812_clear();
        } else {
            led_ws2812_set_color(red, green, blue);
        }

        if ((now - last_log) >= pdMS_TO_TICKS(500)) {
            ESP_LOGI(TAG, "mode=JOYSTICK raw=(%d,%d) norm=(%d,%d) span=(%d,%d) sw=%d -> rgb=(%u,%u,%u) br=%u",
                     x_raw, y_raw, x_norm, y_norm, axis_span(&x_state), axis_span(&y_state), (int)sw_pressed, red, green, blue, brightness);
            last_log = now;
        }

        vTaskDelay(pdMS_TO_TICKS(60));
    }
}