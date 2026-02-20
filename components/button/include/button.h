#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef void (*button_event_handler_t)(void *user_data);

typedef struct button_component *button_handle_t;

typedef struct {
	button_event_handler_t on_press;
	button_event_handler_t on_long_press;
	button_event_handler_t on_click;
	button_event_handler_t on_double_click;
	void *user_data;
} button_callbacks_t;

typedef struct {
	int gpio_num;
	uint8_t active_level;
	uint16_t poll_ms;
	uint16_t debounce_ms;
	uint16_t long_press_ms;
	uint16_t double_click_ms;
} button_config_t;

#define BUTTON_COMPONENT_CONFIG_DEFAULT(gpio, level) \
	(button_config_t){                              \
		.gpio_num = (gpio),                         \
		.active_level = (level),                    \
		.poll_ms = 10,                              \
		.debounce_ms = 30,                          \
		.long_press_ms = 1000,                      \
		.double_click_ms = 350,                     \
	}

esp_err_t button_component_create(const button_config_t *config,
							  const button_callbacks_t *callbacks,
							  button_handle_t *out_handle);
void button_component_destroy(button_handle_t handle);
bool button_component_is_initialized(button_handle_t handle);


#endif