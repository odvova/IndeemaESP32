#include "button.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BUTTON";

#define BUTTON_TASK_STACK_SIZE 3072
#define BUTTON_TASK_PRIORITY 5
#define BUTTON_POLL_MS 10
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_LONG_PRESS_MS 1000
#define BUTTON_DOUBLE_CLICK_MS 350

static TaskHandle_t s_button_task = NULL;
static bool s_running = false;
static bool s_initialized = false;
static int s_gpio_num = -1;
static uint8_t s_active_level = 0;
static button_callbacks_t s_callbacks = {0};

static bool button_raw_pressed(void)
{
    return (gpio_get_level(s_gpio_num) == (int)s_active_level);
}

static void call_press_cb(void)
{
    ESP_LOGI(TAG, "BUTTON_PRESS");
    if (s_callbacks.on_press) {
        s_callbacks.on_press(s_callbacks.user_data);
    }
}

static void call_long_press_cb(void)
{
    ESP_LOGI(TAG, "BUTTON_LONG_PRESS");
    if (s_callbacks.on_long_press) {
        s_callbacks.on_long_press(s_callbacks.user_data);
    }
}

static void call_click_cb(void)
{
    ESP_LOGI(TAG, "BUTTON_CLICK");
    if (s_callbacks.on_click) {
        s_callbacks.on_click(s_callbacks.user_data);
    }
}

static void call_double_click_cb(void)
{
    ESP_LOGI(TAG, "BUTTON_DOUBLE_CLICK");
    if (s_callbacks.on_double_click) {
        s_callbacks.on_double_click(s_callbacks.user_data);
    }
}

static void button_task(void *arg)
{
    (void)arg;

    bool raw_state = button_raw_pressed();
    bool debounced_state = raw_state;
    TickType_t raw_change_tick = xTaskGetTickCount();
    TickType_t press_start_tick = 0;
    TickType_t last_release_tick = 0;
    bool long_press_sent = false;
    uint8_t click_count = 0;

    while (s_running) {
        TickType_t now = xTaskGetTickCount();
        bool raw_now = button_raw_pressed();

        if (raw_now != raw_state) {
            raw_state = raw_now;
            raw_change_tick = now;
        }

        if ((now - raw_change_tick) >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS) && debounced_state != raw_state) {
            debounced_state = raw_state;

            if (debounced_state) {
                press_start_tick = now;
                long_press_sent = false;
                call_press_cb();
            } else {
                if (!long_press_sent) {
                    click_count++;
                    last_release_tick = now;
                }
            }
        }

        if (debounced_state && !long_press_sent &&
            (now - press_start_tick) >= pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS)) {
            long_press_sent = true;
            click_count = 0;
            call_long_press_cb();
        }

        if (!debounced_state && click_count > 0 &&
            (now - last_release_tick) >= pdMS_TO_TICKS(BUTTON_DOUBLE_CLICK_MS)) {
            if (click_count == 1) {
                call_click_cb();
            } else {
                call_double_click_cb();
            }
            click_count = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }

    s_button_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t button_component_init(int gpio_num, uint8_t active_level, const button_callbacks_t *callbacks)
{
    if (callbacks == NULL || gpio_num < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized) {
        ESP_LOGW(TAG, "Button already initialized");
        return ESP_OK;
    }

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (active_level == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (active_level == 0) ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %d (%s)", gpio_num, esp_err_to_name(ret));
        return ret;
    }

    s_gpio_num = gpio_num;
    s_active_level = active_level;
    s_callbacks = *callbacks;
    s_running = true;

    BaseType_t task_ok = xTaskCreate(button_task,
                                     "button_task",
                                     BUTTON_TASK_STACK_SIZE,
                                     NULL,
                                     BUTTON_TASK_PRIORITY,
                                     &s_button_task);
    if (task_ok != pdPASS) {
        s_running = false;
        s_button_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Button initialized on GPIO %d", gpio_num);
    return ESP_OK;
}

void button_component_deinit(void)
{
    if (s_initialized) {
        s_running = false;
        ESP_LOGI(TAG, "Button deinitialized");
        s_initialized = false;
    }
}

bool button_component_is_initialized(void)
{
    return s_initialized;
}
