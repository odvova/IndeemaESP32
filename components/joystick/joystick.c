#include <stdio.h>
#include "joystick.h"

static void configure_joystick(void){
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    configure_adc_pin(JOY_X_GPIO, &s_chan_x);
    configure_adc_pin(JOY_Y_GPIO, &s_chan_y);

    gpio_config_t sw_cfg = {
        .pin_bit_mask = 1ULL << JOY_SW_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sw_cfg));
}

static void read_joystick(int *x, int *y, bool *sw_pressed){
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc, s_chan_x, x));
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc, s_chan_y, y));
    *sw_pressed = (gpio_get_level(JOY_SW_GPIO) == 0);
}

static void configure_adc_pin(int gpio, adc_channel_t *out_channel){
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;

    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(gpio, &unit, &channel));
    if (unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO %d mapped to ADC%d. Use ADC1 pins for joystick.", gpio, unit + 1);
        abort();
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, channel, &chan_cfg));
    *out_channel = channel;
}

