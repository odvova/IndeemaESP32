#include "button.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

static const char *TAG = "BUTTON";

#define BUTTON_TASK_STACK_SIZE 3072
#define BUTTON_TASK_PRIORITY 5
typedef struct button_component {
    TaskHandle_t task;
    bool running;
    bool initialized;
    button_config_t config;
    button_callbacks_t callbacks;
} button_component_t;

static bool button_raw_pressed(const button_component_t *ctx)
{
    return (gpio_get_level(ctx->config.gpio_num) == (int)ctx->config.active_level);
}

static void call_press_cb(button_component_t *ctx)
{
    ESP_LOGI(TAG, "GPIO%d BUTTON_PRESS", ctx->config.gpio_num);
    if (ctx->callbacks.on_press) {
        ctx->callbacks.on_press(ctx->callbacks.user_data);
    }
}

static void call_long_press_cb(button_component_t *ctx)
{
    ESP_LOGI(TAG, "GPIO%d BUTTON_LONG_PRESS", ctx->config.gpio_num);
    if (ctx->callbacks.on_long_press) {
        ctx->callbacks.on_long_press(ctx->callbacks.user_data);
    }
}

static void call_click_cb(button_component_t *ctx)
{
    ESP_LOGI(TAG, "GPIO%d BUTTON_CLICK", ctx->config.gpio_num);
    if (ctx->callbacks.on_click) {
        ctx->callbacks.on_click(ctx->callbacks.user_data);
    }
}

static void call_double_click_cb(button_component_t *ctx)
{
    ESP_LOGI(TAG, "GPIO%d BUTTON_DOUBLE_CLICK", ctx->config.gpio_num);
    if (ctx->callbacks.on_double_click) {
        ctx->callbacks.on_double_click(ctx->callbacks.user_data);
    }
}

static void button_task(void *arg)
{
    button_component_t *ctx = (button_component_t *)arg;

    bool raw_state = button_raw_pressed(ctx);
    bool debounced_state = raw_state;
    TickType_t raw_change_tick = xTaskGetTickCount();
    TickType_t press_start_tick = 0;
    TickType_t last_release_tick = 0;
    bool long_press_sent = false;
    uint8_t click_count = 0;

    while (ctx->running) {
        TickType_t now = xTaskGetTickCount();
        bool raw_now = button_raw_pressed(ctx);

        if (raw_now != raw_state) {
            raw_state = raw_now;
            raw_change_tick = now;
        }

        if ((now - raw_change_tick) >= pdMS_TO_TICKS(ctx->config.debounce_ms) && debounced_state != raw_state) {
            debounced_state = raw_state;

            if (debounced_state) {
                press_start_tick = now;
                long_press_sent = false;
                call_press_cb(ctx);
            } else {
                if (!long_press_sent) {
                    click_count++;
                    last_release_tick = now;
                }
            }
        }

        if (debounced_state && !long_press_sent &&
            (now - press_start_tick) >= pdMS_TO_TICKS(ctx->config.long_press_ms)) {
            long_press_sent = true;
            click_count = 0;
            call_long_press_cb(ctx);
        }

        if (!debounced_state && click_count > 0 &&
            (now - last_release_tick) >= pdMS_TO_TICKS(ctx->config.double_click_ms)) {
            if (click_count == 1) {
                call_click_cb(ctx);
            } else {
                call_double_click_cb(ctx);
            }
            click_count = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(ctx->config.poll_ms));
    }

    ctx->task = NULL;
    vTaskDelete(NULL);
}

esp_err_t button_component_create(const button_config_t *config,
                                  const button_callbacks_t *callbacks,
                                  button_handle_t *out_handle)
{
    if (config == NULL || callbacks == NULL || out_handle == NULL || config->gpio_num < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->poll_ms == 0 || config->debounce_ms == 0 || config->double_click_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    button_component_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ctx->config = *config;
    ctx->callbacks = *callbacks;

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << config->gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (config->active_level == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (config->active_level == 0) ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %d (%s)", config->gpio_num, esp_err_to_name(ret));
        free(ctx);
        return ret;
    }

    ctx->running = true;

    BaseType_t task_ok = xTaskCreate(button_task,
                                     "button_task",
                                     BUTTON_TASK_STACK_SIZE,
                                     ctx,
                                     BUTTON_TASK_PRIORITY,
                                     &ctx->task);
    if (task_ok != pdPASS) {
        ctx->running = false;
        ctx->task = NULL;
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    ctx->initialized = true;
    *out_handle = ctx;
    ESP_LOGI(TAG, "Button initialized on GPIO %d", config->gpio_num);
    return ESP_OK;
}

void button_component_destroy(button_handle_t handle)
{
    button_component_t *ctx = (button_component_t *)handle;
    if (ctx == NULL || !ctx->initialized) {
        return;
    }

    ctx->running = false;
    while (ctx->task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG, "Button deinitialized on GPIO %d", ctx->config.gpio_num);
    ctx->initialized = false;
    free(ctx);
}

bool button_component_is_initialized(button_handle_t handle)
{
    button_component_t *ctx = (button_component_t *)handle;
    return (ctx != NULL && ctx->initialized);
}
