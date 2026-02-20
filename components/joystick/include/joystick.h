#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <stdbool.h>
#include "esp_adc/adc_oneshot.h"

void configure_joystick(void);

void read_joystick(int *x, int *y, bool *sw_pressed);

#endif