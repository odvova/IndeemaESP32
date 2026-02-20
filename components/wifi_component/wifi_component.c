#include "wifi_component.h"

#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "led_WS2812.h"

static const char *TAG = "WIFI_COMPONENT";

#define WIFI_LED_BLINK_MS 500
#define WIFI_TASK_LOOP_MS 100

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64
#define WIFI_HOST_MAX_LEN 64

typedef struct {
    wifi_component_config_t config;
    bool started;
    bool connecting;
    bool sta_has_ip;
    bool sta_error;
    bool internet_ok;
    bool sta_configured;
    bool ap_started;
    int ap_clients;
    wifi_led_state_t state;
    wifi_component_mode_t mode;
    bool sntp_started;
    bool time_synced;
    bool blink_on;
    TickType_t last_blink_tick;
    TickType_t last_internet_check_tick;
    TickType_t next_retry_tick;
    TaskHandle_t task;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;

    char sta_ssid[WIFI_SSID_MAX_LEN + 1];
    char sta_password[WIFI_PASS_MAX_LEN + 1];
    char ap_ssid[WIFI_SSID_MAX_LEN + 1];
    char ap_password[WIFI_PASS_MAX_LEN + 1];
    char internet_host[WIFI_HOST_MAX_LEN + 1];
    bool manual_led_override;
    uint8_t manual_led_red;
    uint8_t manual_led_green;
    uint8_t manual_led_blue;
    TickType_t manual_led_until_tick;
} wifi_component_runtime_t;

static wifi_component_runtime_t s_runtime = {0};

static bool string_is_empty(const char *value)
{
    return (value == NULL || value[0] == '\0');
}

static bool is_sta_active(void)
{
    return (s_runtime.mode == WIFI_COMPONENT_MODE_STA || s_runtime.mode == WIFI_COMPONENT_MODE_APSTA);
}

static bool is_ap_active(void)
{
    return (s_runtime.mode == WIFI_COMPONENT_MODE_AP || s_runtime.mode == WIFI_COMPONENT_MODE_APSTA);
}

static const char *mode_to_string(wifi_component_mode_t mode)
{
    switch (mode) {
        case WIFI_COMPONENT_MODE_AP:
            return "AP";
        case WIFI_COMPONENT_MODE_STA:
            return "STA";
        case WIFI_COMPONENT_MODE_APSTA:
            return "APSTA";
        default:
            return "UNKNOWN";
    }
}

static wifi_led_state_t compute_led_state(void)
{
    if (!s_runtime.started) {
        return WIFI_LED_STATE_OFF;
    }

    if (is_ap_active() && s_runtime.ap_clients > 0) {
        return WIFI_LED_STATE_AP_CLIENT_CONNECTED;
    }

    if (is_sta_active() && s_runtime.sta_configured) {
        if (s_runtime.sta_error) {
            return WIFI_LED_STATE_STA_ERROR;
        }

        if (s_runtime.connecting) {
            return WIFI_LED_STATE_STA_CONNECTING;
        }

        if (s_runtime.sta_has_ip && !s_runtime.internet_ok) {
            return WIFI_LED_STATE_STA_IP_RECEIVED;
        }

        if (s_runtime.internet_ok) {
            return WIFI_LED_STATE_STA_INTERNET_OK;
        }
    }

    if (is_ap_active() && s_runtime.ap_started) {
        return WIFI_LED_STATE_AP_STARTED;
    }

    return WIFI_LED_STATE_OFF;
}

static void set_led(uint8_t red, uint8_t green, uint8_t blue)
{
    (void)led_ws2812_set_color(red, green, blue);
}

static bool is_manual_led_override_active(TickType_t now)
{
    if (!s_runtime.manual_led_override) {
        return false;
    }

    return ((int32_t)(s_runtime.manual_led_until_tick - now) > 0);
}

static void apply_led_state(wifi_led_state_t state)
{
    TickType_t now = xTaskGetTickCount();

    if (state == WIFI_LED_STATE_STA_IP_RECEIVED || state == WIFI_LED_STATE_AP_STARTED) {
        if ((now - s_runtime.last_blink_tick) >= pdMS_TO_TICKS(WIFI_LED_BLINK_MS)) {
            s_runtime.last_blink_tick = now;
            s_runtime.blink_on = !s_runtime.blink_on;
        }
    } else {
        s_runtime.blink_on = true;
    }

    switch (state) {
        case WIFI_LED_STATE_OFF:
            set_led(32, 32, 32);
            break;
        case WIFI_LED_STATE_STA_CONNECTING:
            set_led(255, 180, 0);
            break;
        case WIFI_LED_STATE_STA_ERROR:
            set_led(255, 0, 0);
            break;
        case WIFI_LED_STATE_STA_IP_RECEIVED:
            if (s_runtime.blink_on) {
                set_led(0, 255, 0);
            } else {
                (void)led_ws2812_clear();
            }
            break;
        case WIFI_LED_STATE_STA_INTERNET_OK:
            set_led(0, 255, 0);
            break;
        case WIFI_LED_STATE_AP_STARTED:
            if (s_runtime.blink_on) {
                set_led(0, 0, 255);
            } else {
                (void)led_ws2812_clear();
            }
            break;
        case WIFI_LED_STATE_AP_CLIENT_CONNECTED:
            set_led(0, 0, 255);
            break;
        default:
            (void)led_ws2812_clear();
            break;
    }
}

static bool check_internet_access(const char *host)
{
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;

    int ret = getaddrinfo(host, "80", &hints, &result);
    if (result != NULL) {
        freeaddrinfo(result);
    }

    return (ret == 0);
}

static bool copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0 || src == NULL) {
        return false;
    }

    size_t len = strlen(src);
    if (len >= dst_size) {
        return false;
    }

    memcpy(dst, src, len + 1);
    return true;
}

static void nvs_save_sta_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("wifi_cred", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

    nvs_set_str(nvs_handle, "sta_ssid", s_runtime.sta_ssid);
    nvs_set_str(nvs_handle, "sta_pass", s_runtime.sta_password);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "STA credentials saved to NVS");
}

static void nvs_load_sta_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("wifi_cred", NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No saved credentials in NVS (first boot?)");
        return;
    }

    size_t ssid_len = sizeof(s_runtime.sta_ssid);
    size_t pass_len = sizeof(s_runtime.sta_password);
    
    esp_err_t ret_ssid = nvs_get_str(nvs_handle, "sta_ssid", s_runtime.sta_ssid, &ssid_len);
    esp_err_t ret_pass = nvs_get_str(nvs_handle, "sta_pass", s_runtime.sta_password, &pass_len);
    
    nvs_close(nvs_handle);

    if (ret_ssid == ESP_OK && ret_pass == ESP_OK && strlen(s_runtime.sta_ssid) > 0) {
        s_runtime.config.sta_ssid = s_runtime.sta_ssid;
        s_runtime.config.sta_password = s_runtime.sta_password;
        s_runtime.sta_configured = true;
        ESP_LOGI(TAG, "STA credentials loaded from NVS: '%s'", s_runtime.sta_ssid);
    }
}

static bool apply_runtime_strings(const wifi_component_config_t *config)
{
    const char *sta_ssid = (config->sta_ssid != NULL) ? config->sta_ssid : "";
    const char *sta_pass = (config->sta_password != NULL) ? config->sta_password : "";
    const char *ap_ssid = (config->ap_ssid != NULL) ? config->ap_ssid : "";
    const char *ap_pass = (config->ap_password != NULL) ? config->ap_password : "";
    const char *host = (config->internet_check_host != NULL) ? config->internet_check_host : "";

    if (!copy_text(s_runtime.sta_ssid, sizeof(s_runtime.sta_ssid), sta_ssid) ||
        !copy_text(s_runtime.sta_password, sizeof(s_runtime.sta_password), sta_pass) ||
        !copy_text(s_runtime.ap_ssid, sizeof(s_runtime.ap_ssid), ap_ssid) ||
        !copy_text(s_runtime.ap_password, sizeof(s_runtime.ap_password), ap_pass) ||
        !copy_text(s_runtime.internet_host, sizeof(s_runtime.internet_host), host)) {
        return false;
    }

    s_runtime.config.sta_ssid = s_runtime.sta_ssid;
    s_runtime.config.sta_password = s_runtime.sta_password;
    s_runtime.config.ap_ssid = s_runtime.ap_ssid;
    s_runtime.config.ap_password = s_runtime.ap_password;
    s_runtime.config.internet_check_host = s_runtime.internet_host;
    s_runtime.sta_configured = !string_is_empty(s_runtime.sta_ssid);
    return true;
}

static void sntp_sync_check(void)
{
    if (!s_runtime.sntp_started && s_runtime.internet_ok && s_runtime.sta_has_ip) {
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&sntp_cfg) == ESP_OK) {
            esp_netif_sntp_start();
            s_runtime.sntp_started = true;
            ESP_LOGI(TAG, "SNTP started");
        }
    }

    if (s_runtime.sntp_started && !s_runtime.time_synced) {
        time_t now = 0;
        time(&now);
        if (now > 1700000000) {
            s_runtime.time_synced = true;
            char time_str[64];
            struct tm *timeinfo = localtime(&now);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
            ESP_LOGI(TAG, "SNTP time synchronized: %s (timestamp: %ld)", time_str, now);
        }
    }
}

static esp_err_t configure_sta(const wifi_component_config_t *config)
{
    if (string_is_empty(config->sta_ssid)) {
        ESP_LOGW(TAG, "STA SSID is empty; STA connection disabled");
        s_runtime.sta_configured = false;
        return ESP_OK;
    }

    wifi_config_t sta_cfg = {0};
    size_t ssid_len = strlen(config->sta_ssid);
    size_t pass_len = string_is_empty(config->sta_password) ? 0 : strlen(config->sta_password);

    if (ssid_len > sizeof(sta_cfg.sta.ssid) - 1 || pass_len > sizeof(sta_cfg.sta.password) - 1) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(sta_cfg.sta.ssid, config->sta_ssid, ssid_len);
    if (pass_len > 0) {
        memcpy(sta_cfg.sta.password, config->sta_password, pass_len);
    }

    sta_cfg.sta.threshold.authmode = (pass_len == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    s_runtime.sta_configured = true;
    return esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
}

static esp_err_t configure_ap(const wifi_component_config_t *config)
{
    if (string_is_empty(config->ap_ssid)) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t ap_cfg = {0};
    size_t ssid_len = strlen(config->ap_ssid);
    size_t pass_len = string_is_empty(config->ap_password) ? 0 : strlen(config->ap_password);

    if (ssid_len > sizeof(ap_cfg.ap.ssid) - 1 || pass_len > sizeof(ap_cfg.ap.password) - 1) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(ap_cfg.ap.ssid, config->ap_ssid, ssid_len);
    ap_cfg.ap.ssid_len = (uint8_t)ssid_len;
    ap_cfg.ap.channel = config->ap_channel;
    ap_cfg.ap.max_connection = config->ap_max_connections;

    if (pass_len == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        memcpy(ap_cfg.ap.password, config->ap_password, pass_len);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    return esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
}

static wifi_mode_t mode_to_idf(wifi_component_mode_t mode)
{
    switch (mode) {
        case WIFI_COMPONENT_MODE_AP:
            return WIFI_MODE_AP;
        case WIFI_COMPONENT_MODE_STA:
            return WIFI_MODE_STA;
        case WIFI_COMPONENT_MODE_APSTA:
            return WIFI_MODE_APSTA;
        default:
            return WIFI_MODE_APSTA;
    }
}

static void reset_sta_state(void)
{
    s_runtime.connecting = false;
    s_runtime.sta_has_ip = false;
    s_runtime.sta_error = false;
    s_runtime.internet_ok = false;
    s_runtime.time_synced = false;
}



static void wifi_component_task(void *arg)
{
    (void)arg;
    TickType_t last_time_log_tick = xTaskGetTickCount();

    while (s_runtime.started) {
        TickType_t now = xTaskGetTickCount();

        if (is_sta_active() &&
            s_runtime.sta_configured &&
            s_runtime.sta_error &&
            !s_runtime.connecting &&
            now >= s_runtime.next_retry_tick) {
            s_runtime.sta_error = false;
            s_runtime.connecting = true;
            s_runtime.internet_ok = false;
            s_runtime.state = compute_led_state();
            ESP_LOGI(TAG, "STA retrying connection...");
            (void)esp_wifi_connect();
        }

        if (is_sta_active() &&
            s_runtime.sta_configured &&
            s_runtime.sta_has_ip &&
            !s_runtime.connecting &&
            (now - s_runtime.last_internet_check_tick) >= pdMS_TO_TICKS(s_runtime.config.internet_check_interval_ms)) {
            s_runtime.last_internet_check_tick = now;
            s_runtime.internet_ok = check_internet_access(s_runtime.config.internet_check_host);
        }

        sntp_sync_check();

        if ((now - last_time_log_tick) >= pdMS_TO_TICKS(5000)) {
            last_time_log_tick = now;
            time_t current_time = time(NULL);
            char time_str[64];
            struct tm *timeinfo = localtime(&current_time);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
            ESP_LOGI(TAG, "[TIME] %s (SNTP: %s)", time_str, s_runtime.time_synced ? "YES" : "NO");
        }

        wifi_led_state_t new_state = compute_led_state();
        if (new_state != s_runtime.state) {
            s_runtime.state = new_state;
            ESP_LOGI(TAG, "LED state changed to %d", (int)s_runtime.state);
        }

        if (is_manual_led_override_active(now)) {
            set_led(s_runtime.manual_led_red, s_runtime.manual_led_green, s_runtime.manual_led_blue);
        } else {
            if (s_runtime.manual_led_override) {
                s_runtime.manual_led_override = false;
                ESP_LOGI(TAG, "Manual LED override expired; resuming Wi-Fi LED state");
            }
            apply_led_state(s_runtime.state);
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_TASK_LOOP_MS));
    }

    s_runtime.task = NULL;
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                if (is_sta_active() && s_runtime.sta_configured) {
                    s_runtime.connecting = true;
                    s_runtime.sta_error = false;
                    s_runtime.internet_ok = false;
                    (void)esp_wifi_connect();
                }
                break;

            case WIFI_EVENT_STA_CONNECTED:
                s_runtime.connecting = false;
                s_runtime.sta_error = false;
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                if (is_sta_active() && s_runtime.sta_configured) {
                    s_runtime.connecting = false;
                    s_runtime.sta_has_ip = false;
                    s_runtime.internet_ok = false;
                    s_runtime.sta_error = true;
                    s_runtime.next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(s_runtime.config.sta_retry_interval_ms);
                    ESP_LOGW(TAG, "STA disconnected; scheduling retry in %lu ms", (unsigned long)s_runtime.config.sta_retry_interval_ms);
                }
                break;

            case WIFI_EVENT_AP_START:
                s_runtime.ap_started = true;
                break;

            case WIFI_EVENT_AP_STOP:
                s_runtime.ap_started = false;
                s_runtime.ap_clients = 0;
                break;

            case WIFI_EVENT_AP_STACONNECTED:
                s_runtime.ap_clients++;
                break;

            case WIFI_EVENT_AP_STADISCONNECTED:
                if (s_runtime.ap_clients > 0) {
                    s_runtime.ap_clients--;
                }
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            s_runtime.sta_has_ip = true;
            s_runtime.connecting = false;
            s_runtime.sta_error = false;
            s_runtime.internet_ok = false;
            s_runtime.last_internet_check_tick = 0;
            ESP_LOGI(TAG, "STA got IP");
        }
    }
}

esp_err_t wifi_component_start(const wifi_component_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_runtime.started) {
        return ESP_OK;
    }

    if (config->internet_check_interval_ms == 0 || config->sta_retry_interval_ms == 0 ||
        string_is_empty(config->internet_check_host)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.config = *config;

    if (!apply_runtime_strings(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_runtime.mode = config->initial_mode;
    s_runtime.state = WIFI_LED_STATE_OFF;
    s_runtime.blink_on = true;
    s_runtime.next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(config->sta_retry_interval_ms);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_load_sta_credentials();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_runtime.sta_netif = esp_netif_create_default_wifi_sta();
    s_runtime.ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &s_runtime.wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &s_runtime.ip_handler));

    ESP_ERROR_CHECK(esp_wifi_set_mode(mode_to_idf(s_runtime.mode)));
    
    // Only configure STA if STA mode is active and credentials are available
    if (is_sta_active() && s_runtime.sta_configured) {
        esp_err_t ret = configure_sta(&s_runtime.config);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to configure STA: %s", esp_err_to_name(ret));
        }
    }
    
    ESP_ERROR_CHECK(configure_ap(&s_runtime.config));

    ESP_ERROR_CHECK(led_ws2812_init(config->led_gpio));

    s_runtime.started = true;
    s_runtime.state = compute_led_state();

    BaseType_t created = xTaskCreate(wifi_component_task,
                                     "wifi_component_task",
                                     4096,
                                     NULL,
                                     5,
                                     &s_runtime.task);
    if (created != pdPASS) {
        s_runtime.started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    if (is_sta_active() && s_runtime.sta_configured) {
        s_runtime.connecting = true;
        (void)esp_wifi_connect();
    }
    ESP_LOGI(TAG, "Wi-Fi component started (mode=%s)", mode_to_string(s_runtime.mode));

    return ESP_OK;
}

void wifi_component_stop(void)
{
    if (!s_runtime.started) {
        return;
    }

    s_runtime.started = false;
    while (s_runtime.task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_runtime.ip_handler);
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_runtime.wifi_handler);

    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();

    if (s_runtime.sntp_started) {
        esp_netif_sntp_deinit();
        s_runtime.sntp_started = false;
    }

    (void)led_ws2812_clear();

    s_runtime.state = WIFI_LED_STATE_OFF;
    ESP_LOGI(TAG, "Wi-Fi component stopped");
}

bool wifi_component_is_started(void)
{
    return s_runtime.started;
}

wifi_led_state_t wifi_component_get_state(void)
{
    return s_runtime.state;
}

wifi_component_mode_t wifi_component_get_mode(void)
{
    return s_runtime.mode;
}

esp_err_t wifi_component_set_mode(wifi_component_mode_t mode)
{
    if (mode != WIFI_COMPONENT_MODE_AP && mode != WIFI_COMPONENT_MODE_STA && mode != WIFI_COMPONENT_MODE_APSTA) {
        return ESP_ERR_INVALID_ARG;
    }

    s_runtime.mode = mode;
    if (!s_runtime.started) {
        return ESP_OK;
    }

    esp_err_t ret = esp_wifi_set_mode(mode_to_idf(mode));
    if (ret != ESP_OK) {
        return ret;
    }

    if (is_sta_active() && s_runtime.sta_configured) {
        s_runtime.connecting = true;
        s_runtime.sta_error = false;
        (void)esp_wifi_connect();
    } else {
        reset_sta_state();
        (void)esp_wifi_disconnect();
    }

    if (!is_ap_active()) {
        s_runtime.ap_started = false;
        s_runtime.ap_clients = 0;
    }

    s_runtime.state = compute_led_state();
    ESP_LOGI(TAG, "Mode switched to %s", mode_to_string(mode));
    return ESP_OK;
}

esp_err_t wifi_component_set_sta_credentials(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "set_sta_credentials called: ssid='%s', password='%s'", ssid ? ssid : "NULL", password ? password : "NULL");
    
    if (ssid == NULL || strlen(ssid) > WIFI_SSID_MAX_LEN ||
        (password != NULL && strlen(password) > WIFI_PASS_MAX_LEN)) {
        ESP_LOGW(TAG, "Validation failed: ssid_null=%d, ssid_len=%d, pass_len=%d", 
                 ssid == NULL, ssid ? (int)strlen(ssid) : 0, password ? (int)strlen(password) : 0);
        return ESP_ERR_INVALID_ARG;
    }

    if (!copy_text(s_runtime.sta_ssid, sizeof(s_runtime.sta_ssid), ssid) ||
        !copy_text(s_runtime.sta_password, sizeof(s_runtime.sta_password), (password != NULL) ? password : "")) {
        ESP_LOGW(TAG, "copy_text failed");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Credentials copied to runtime: ssid='%s', pass_len=%d", 
             s_runtime.sta_ssid, (int)strlen(s_runtime.sta_password));

    s_runtime.config.sta_ssid = s_runtime.sta_ssid;
    s_runtime.config.sta_password = s_runtime.sta_password;
    s_runtime.sta_configured = !string_is_empty(s_runtime.sta_ssid);

    // Save credentials to NVS for persistence across reboots
    if (s_runtime.sta_configured) {
        nvs_save_sta_credentials();
    }

    if (!s_runtime.started) {
        ESP_LOGI(TAG, "Component not started yet, storing credentials only");
        return ESP_OK;
    }

    // If we're in AP-only mode, switch to AP+STA to allow STA configuration
    if (s_runtime.mode == WIFI_COMPONENT_MODE_AP) {
        ESP_LOGI(TAG, "Currently in AP mode, switching to AP+STA to configure STA");
        esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to switch to AP+STA mode: %s", esp_err_to_name(ret));
            return ret;
        }
        s_runtime.mode = WIFI_COMPONENT_MODE_APSTA;
    }

    esp_err_t ret = configure_sta(&s_runtime.config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "configure_sta failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (is_sta_active() && s_runtime.sta_configured) {
        s_runtime.connecting = true;
        s_runtime.sta_error = false;
        s_runtime.sta_has_ip = false;
        s_runtime.internet_ok = false;
        s_runtime.time_synced = false;
        ESP_LOGI(TAG, "STA credentials updated, will initiate connection on next task cycle");
        if (s_runtime.mode != WIFI_COMPONENT_MODE_APSTA) {
            return esp_wifi_connect();
        }
    }

    s_runtime.state = compute_led_state();
    return ESP_OK;
}

bool wifi_component_time_is_synced(void)
{
    return s_runtime.time_synced;
}

bool wifi_component_has_ip(void)
{
    return s_runtime.sta_has_ip;
}

int8_t wifi_component_get_rssi(void)
{
    if (!s_runtime.sta_has_ip) {
        return 0;
    }
    
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret != ESP_OK) {
        return 0;
    }
    
    return ap_info.rssi;
}

bool wifi_component_is_sntp_synced(void)
{
    return s_runtime.time_synced;
}

const char *wifi_component_get_led_state_name(void)
{
    switch (s_runtime.state) {
        case WIFI_LED_STATE_OFF:
            return "off";
        case WIFI_LED_STATE_STA_CONNECTING:
            return "sta_connecting";
        case WIFI_LED_STATE_STA_ERROR:
            return "sta_error";
        case WIFI_LED_STATE_STA_IP_RECEIVED:
            return "sta_ip_received";
        case WIFI_LED_STATE_STA_INTERNET_OK:
            return "sta_internet_ok";
        case WIFI_LED_STATE_AP_STARTED:
            return "ap_started";
        case WIFI_LED_STATE_AP_CLIENT_CONNECTED:
            return "ap_client_connected";
        default:
            return "unknown";
    }
}

esp_err_t wifi_component_set_led_override(uint8_t red, uint8_t green, uint8_t blue, uint32_t hold_ms)
{
    if (!s_runtime.started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (hold_ms == 0) {
        s_runtime.manual_led_override = false;
        return ESP_OK;
    }

    s_runtime.manual_led_red = red;
    s_runtime.manual_led_green = green;
    s_runtime.manual_led_blue = blue;
    s_runtime.manual_led_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(hold_ms);
    s_runtime.manual_led_override = true;

    set_led(red, green, blue);
    return ESP_OK;
}
