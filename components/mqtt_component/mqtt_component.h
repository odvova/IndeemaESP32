#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    MQTT_STATE_DISCONNECTED,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_RECONNECTING,
} mqtt_state_t;


void mqtt_component_init(void);
void mqtt_component_start(void);
void mqtt_component_stop(void);
mqtt_state_t mqtt_component_get_state(void);
bool mqtt_component_is_connected(void);
void mqtt_component_publish_telemetry(void);
