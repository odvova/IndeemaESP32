#ifndef WIFI_COMPONENT_H
#define WIFI_COMPONENT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

typedef enum {
    WIFI_LED_STATE_OFF = 0,
    WIFI_LED_STATE_STA_CONNECTING,
    WIFI_LED_STATE_STA_ERROR,
    WIFI_LED_STATE_STA_IP_RECEIVED,
    WIFI_LED_STATE_STA_INTERNET_OK,
    WIFI_LED_STATE_AP_STARTED,
    WIFI_LED_STATE_AP_CLIENT_CONNECTED,
} wifi_led_state_t;

typedef enum {
    WIFI_COMPONENT_MODE_AP = 0,
    WIFI_COMPONENT_MODE_STA,
    WIFI_COMPONENT_MODE_APSTA,
} wifi_component_mode_t;

typedef struct {
    bool enable_sta;
    bool enable_ap;
    const char *sta_ssid;
    const char *sta_password;
    const char *ap_ssid;
    const char *ap_password;
    uint8_t ap_channel;
    uint8_t ap_max_connections;
    wifi_component_mode_t initial_mode;
    uint32_t sta_retry_interval_ms;
    uint32_t internet_check_interval_ms;
    const char *internet_check_host;
    int led_gpio;
} wifi_component_config_t;

#define WIFI_COMPONENT_CONFIG_DEFAULT()                 \
    (wifi_component_config_t){                          \
        .enable_sta = true,                             \
        .enable_ap = true,                              \
        .sta_ssid = CONFIG_WIFI_COMPONENT_STA_SSID,     \
        .sta_password = CONFIG_WIFI_COMPONENT_STA_PASS, \
        .ap_ssid = CONFIG_WIFI_COMPONENT_AP_SSID,       \
        .ap_password = CONFIG_WIFI_COMPONENT_AP_PASS,   \
        .ap_channel = 1,                                \
        .ap_max_connections = 4,                        \
        .initial_mode = WIFI_COMPONENT_MODE_APSTA,      \
        .sta_retry_interval_ms = 5000,                  \
        .internet_check_interval_ms = 10000,            \
        .internet_check_host = "connectivitycheck.gstatic.com", \
        .led_gpio = CONFIG_BLINK_GPIO,                  \
    }

esp_err_t wifi_component_start(const wifi_component_config_t *config);
void wifi_component_stop(void);
bool wifi_component_is_started(void);
wifi_led_state_t wifi_component_get_state(void);
wifi_component_mode_t wifi_component_get_mode(void);
esp_err_t wifi_component_set_mode(wifi_component_mode_t mode);
esp_err_t wifi_component_set_sta_credentials(const char *ssid, const char *password);
bool wifi_component_time_is_synced(void);

#endif
