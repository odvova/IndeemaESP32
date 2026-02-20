#include <stdio.h>
#include "joystick.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#define ADC_MAX_VALUE 4095
#define ZONE_LOW_TO_MID 1365
#define ZONE_MID_TO_HIGH 2730
#define ZONE_HYSTERESIS 120
#define MIN_EFFECTIVE_SPAN 20

#ifndef JOY_X_GPIO
#define JOY_X_GPIO 4
#endif

#ifndef JOY_Y_GPIO
#define JOY_Y_GPIO 5
#endif

#ifndef JOY_SW_GPIO
#define JOY_SW_GPIO 6
#endif

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_chan_x;
static adc_channel_t s_chan_y;
static bool s_is_ready = false;

static const char *TAG = "JOYSTICK";

static esp_err_t configure_adc_pin(int gpio, adc_channel_t *out_channel)
{
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;

    esp_err_t ret = adc_oneshot_io_to_channel(gpio, &unit, &channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d is not a valid ADC pin (%s)", gpio, esp_err_to_name(ret));
        return ret;
    }

    if (unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO %d mapped to ADC%d. Use ADC1 pins for joystick.", gpio, unit + 1);
        return ESP_ERR_INVALID_ARG;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ret = adc_oneshot_config_channel(s_adc, channel, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel for GPIO %d (%s)", gpio, esp_err_to_name(ret));
        return ret;
    }

    *out_channel = channel;
    return ESP_OK;
}

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

static int normalize_axis(joystick_axis_state_t *state, int sample)
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

static int axis_span(const joystick_axis_state_t *state)
{
    if (!state->initialized) {
        return 0;
    }
    return state->max_seen - state->min_seen;
}

int joystick_get_switch_gpio(void)
{
    return JOY_SW_GPIO;
}

esp_err_t joystick_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ADC unit (%s)", esp_err_to_name(ret));
        return ret;
    }

    ret = configure_adc_pin(JOY_X_GPIO, &s_chan_x);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = configure_adc_pin(JOY_Y_GPIO, &s_chan_y);
    if (ret != ESP_OK) {
        return ret;
    }

    gpio_config_t sw_cfg = {
        .pin_bit_mask = 1ULL << JOY_SW_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&sw_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure switch GPIO %d (%s)", JOY_SW_GPIO, esp_err_to_name(ret));
        return ret;
    }

    s_is_ready = true;
    ESP_LOGI(TAG, "Joystick configured: X=GPIO%d Y=GPIO%d SW=GPIO%d", JOY_X_GPIO, JOY_Y_GPIO, JOY_SW_GPIO);
    return ESP_OK;
}

esp_err_t joystick_read_sample(joystick_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sample->x = ADC_MAX_VALUE / 2;
    sample->y = ADC_MAX_VALUE / 2;
    sample->sw_pressed = false;

    if (!s_is_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (adc_oneshot_read(s_adc, s_chan_x, &sample->x) != ESP_OK) {
        sample->x = ADC_MAX_VALUE / 2;
    }

    if (adc_oneshot_read(s_adc, s_chan_y, &sample->y) != ESP_OK) {
        sample->y = ADC_MAX_VALUE / 2;
    }

    sample->sw_pressed = (gpio_get_level(JOY_SW_GPIO) == 0);
    return ESP_OK;
}

void joystick_runtime_reset(joystick_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    runtime->x_axis = (joystick_axis_state_t){0};
    runtime->y_axis = (joystick_axis_state_t){0};
    runtime->color_zone = 1;
    runtime->brightness_zone = 1;
    runtime->initialized = true;
}

void joystick_map_sample_to_led(joystick_runtime_t *runtime, const joystick_sample_t *sample, joystick_led_state_t *led_state)
{
    if (runtime == NULL || sample == NULL || led_state == NULL) {
        return;
    }

    if (!runtime->initialized) {
        joystick_runtime_reset(runtime);
    }

    led_state->x_raw = sample->x;
    led_state->y_raw = sample->y;
    led_state->sw_pressed = sample->sw_pressed;
    led_state->x_norm = normalize_axis(&runtime->x_axis, sample->x);
    led_state->y_norm = normalize_axis(&runtime->y_axis, sample->y);

    runtime->color_zone = zone_from_norm(led_state->x_norm, runtime->color_zone);
    led_state->red = 0;
    led_state->green = 0;
    led_state->blue = 0;
    if (runtime->color_zone == 0) {
        led_state->red = 255;
    } else if (runtime->color_zone == 1) {
        led_state->green = 255;
    } else {
        led_state->blue = 255;
    }

    led_state->x_span = axis_span(&runtime->x_axis);
    led_state->y_span = axis_span(&runtime->y_axis);
    if (led_state->y_span < MIN_EFFECTIVE_SPAN) {
        runtime->brightness_zone = 1;
    } else {
        runtime->brightness_zone = zone_from_norm(led_state->y_norm, runtime->brightness_zone);
    }

    if (runtime->brightness_zone == 0) {
        led_state->brightness = 0;
    } else if (runtime->brightness_zone == 1) {
        led_state->brightness = 128;
    } else {
        led_state->brightness = 255;
    }

    led_state->red = (uint8_t)((led_state->red * led_state->brightness) / 255);
    led_state->green = (uint8_t)((led_state->green * led_state->brightness) / 255);
    led_state->blue = (uint8_t)((led_state->blue * led_state->brightness) / 255);
    led_state->led_on = (led_state->brightness > 0);
}

void configure_joystick(void)
{
    (void)joystick_init();
}

void read_joystick(int *x, int *y, bool *sw_pressed)
{
    joystick_sample_t sample = {0};
    if (x == NULL || y == NULL || sw_pressed == NULL) {
        return;
    }

    (void)joystick_read_sample(&sample);
    *x = sample.x;
    *y = sample.y;
    *sw_pressed = sample.sw_pressed;
}


