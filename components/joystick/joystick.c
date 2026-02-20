#include <stdio.h>
#include "joystick.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

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


void configure_joystick(void){
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ADC unit (%s)", esp_err_to_name(ret));
        return;
    }

    ret = configure_adc_pin(JOY_X_GPIO, &s_chan_x);
    if (ret != ESP_OK) {
        return;
    }

    ret = configure_adc_pin(JOY_Y_GPIO, &s_chan_y);
    if (ret != ESP_OK) {
        return;
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
        return;
    }

    s_is_ready = true;
    ESP_LOGI(TAG, "Joystick configured: X=GPIO%d Y=GPIO%d SW=GPIO%d", JOY_X_GPIO, JOY_Y_GPIO, JOY_SW_GPIO);
}

void read_joystick(int *x, int *y, bool *sw_pressed){
    if (!s_is_ready) {
        *x = 2048;
        *y = 2048;
        *sw_pressed = false;
        return;
    }

    if (adc_oneshot_read(s_adc, s_chan_x, x) != ESP_OK) {
        *x = 2048;
    }

    if (adc_oneshot_read(s_adc, s_chan_y, y) != ESP_OK) {
        *y = 2048;
    }

    *sw_pressed = (gpio_get_level(JOY_SW_GPIO) == 0);
}


