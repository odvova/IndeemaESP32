#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef void (*button_event_handler_t)(void *user_data);

typedef struct {
	button_event_handler_t on_press;
	button_event_handler_t on_long_press;
	button_event_handler_t on_click;
	button_event_handler_t on_double_click;
	void *user_data;
} button_callbacks_t;

esp_err_t button_component_init(int gpio_num, uint8_t active_level, const button_callbacks_t *callbacks);
void button_component_deinit(void);
bool button_component_is_initialized(void);


#endif