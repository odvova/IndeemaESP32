#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
	int x;
	int y;
	bool sw_pressed;
} joystick_sample_t;

typedef struct {
	int min_seen;
	int max_seen;
	int filtered;
	bool initialized;
} joystick_axis_state_t;

typedef struct {
	joystick_axis_state_t x_axis;
	joystick_axis_state_t y_axis;
	int color_zone;
	int brightness_zone;
	bool initialized;
} joystick_runtime_t;

typedef struct {
	int x_raw;
	int y_raw;
	int x_norm;
	int y_norm;
	int x_span;
	int y_span;
	bool sw_pressed;
	uint8_t red;
	uint8_t green;
	uint8_t blue;
	uint8_t brightness;
	bool led_on;
} joystick_led_state_t;

esp_err_t joystick_init(void);
int joystick_get_switch_gpio(void);
esp_err_t joystick_read_sample(joystick_sample_t *sample);
void joystick_runtime_reset(joystick_runtime_t *runtime);
void joystick_map_sample_to_led(joystick_runtime_t *runtime, const joystick_sample_t *sample, joystick_led_state_t *led_state);

void configure_joystick(void);
void read_joystick(int *x, int *y, bool *sw_pressed);

#endif