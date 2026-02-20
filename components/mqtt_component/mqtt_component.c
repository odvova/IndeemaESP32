#include "mqtt_component.h"
#include "wifi_component.h"

#include "esp_log.h"
#include "esp_err.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static const char *TAG = "MQTT_COMPONENT";

// CONFIG
#define MQTT_BROKER_URI CONFIG_MQTT_BROKER_URI
#define MQTT_CMD_TOPIC "esp-lection/cmd"
#define MQTT_STATUS_TOPIC "esp-lection/status"
#define TELEMETRY_INTERVAL_MS 5000
#define RECONNECT_BACKOFF_MIN_MS 1000
#define RECONNECT_BACKOFF_MAX_MS 60000
#define MQTT_LED_OVERRIDE_MS 1200

// Command queue item
typedef struct {
    char topic[64];
    char payload[256];
} mqtt_cmd_t;

// Component state
typedef struct {
    esp_mqtt_client_handle_t client;
    mqtt_state_t state;
    TaskHandle_t telemetry_task;
    TaskHandle_t command_task;
    QueueHandle_t command_queue;
    uint32_t reconnect_backoff_ms;
    uint32_t startup_tick;
} mqtt_component_state_t;

static mqtt_component_state_t s_state = {0};

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void telemetry_task(void *arg);
static void command_task(void *arg);

void mqtt_component_init(void) {
    ESP_LOGI(TAG, "Initializing MQTT component");
    
    s_state.command_queue = xQueueCreate(10, sizeof(mqtt_cmd_t));
    if (s_state.command_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return;
    }
    
    s_state.state = MQTT_STATE_DISCONNECTED;
    s_state.reconnect_backoff_ms = RECONNECT_BACKOFF_MIN_MS;
    s_state.startup_tick = xTaskGetTickCount();
    
    xTaskCreate(command_task, "mqtt_cmd_task", 4096, NULL, 5, &s_state.command_task);
    
    ESP_LOGI(TAG, "MQTT component initialized");
}

void mqtt_component_start(void) {
    if (s_state.client != NULL) {
        ESP_LOGW(TAG, "MQTT client already started");
        return;
    }
    
    if (!wifi_component_has_ip()) {
        ESP_LOGW(TAG, "Wi-Fi has no IP, deferring MQTT start");
        return;
    }
    
    ESP_LOGI(TAG, "Starting MQTT client, connecting to %s", MQTT_BROKER_URI);
    
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .session.keepalive = 60,
        .task.stack_size = 4096,
        .task.priority = 5,
    };
    
    s_state.client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_state.client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }
    
    esp_mqtt_client_register_event(s_state.client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    
    esp_err_t err = esp_mqtt_client_start(s_state.client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_state.client);
        s_state.client = NULL;
        return;
    }
    
    s_state.state = MQTT_STATE_CONNECTING;
    ESP_LOGI(TAG, "MQTT client started");
}

void mqtt_component_stop(void) {
    if (s_state.client == NULL) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping MQTT client");
    esp_mqtt_client_stop(s_state.client);
    esp_mqtt_client_destroy(s_state.client);
    s_state.client = NULL;
    s_state.state = MQTT_STATE_DISCONNECTED;
    
    s_state.reconnect_backoff_ms = RECONNECT_BACKOFF_MIN_MS;
    
    if (s_state.telemetry_task != NULL) {
        vTaskDelete(s_state.telemetry_task);
        s_state.telemetry_task = NULL;
    }
    
    ESP_LOGI(TAG, "MQTT client stopped");
}

mqtt_state_t mqtt_component_get_state(void) {
    return s_state.state;
}

bool mqtt_component_is_connected(void) {
    return s_state.state == MQTT_STATE_CONNECTED;
}

void mqtt_component_publish_telemetry(void) {
    if (s_state.client == NULL || s_state.state != MQTT_STATE_CONNECTED) {
        return;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON for telemetry");
        return;
    }
    
    // Get telemetry data
    int8_t rssi = wifi_component_get_rssi();
    bool sntp_synced = wifi_component_is_sntp_synced();
    uint32_t uptime_s = (xTaskGetTickCount() - s_state.startup_tick) / 1000;
    const char *led_state = wifi_component_get_led_state_name();
    
    cJSON_AddNumberToObject(root, "rssi", rssi);
    cJSON_AddBoolToObject(root, "sntp_synced", sntp_synced);
    cJSON_AddNumberToObject(root, "uptime_s", uptime_s);
    cJSON_AddStringToObject(root, "led_state", led_state);
    cJSON_AddStringToObject(root, "status", "online");
    
    char *payload = cJSON_Print(root);
    if (payload == NULL) {
        ESP_LOGE(TAG, "Failed to print JSON");
        cJSON_Delete(root);
        return;
    }
    
    int msg_id = esp_mqtt_client_publish(s_state.client, MQTT_STATUS_TOPIC, payload, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Failed to publish telemetry");
    }
    
    free(payload);
    cJSON_Delete(root);
}


static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            s_state.state = MQTT_STATE_CONNECTED;
            s_state.reconnect_backoff_ms = RECONNECT_BACKOFF_MIN_MS;
            
            esp_mqtt_client_publish(s_state.client, MQTT_STATUS_TOPIC, 
                                   "{\"status\":\"online\"}", 0, 1, 1);
            
            int msg_id = esp_mqtt_client_subscribe(s_state.client, MQTT_CMD_TOPIC, 1);
            if (msg_id < 0) {
                ESP_LOGW(TAG, "Failed to subscribe to command topic");
            } else {
                ESP_LOGI(TAG, "Subscribed to %s (msg_id=%d)", MQTT_CMD_TOPIC, msg_id);
            }
            
            if (s_state.telemetry_task == NULL) {
                xTaskCreate(telemetry_task, "mqtt_telemetry", 3072, NULL, 5, &s_state.telemetry_task);
            }
            
            mqtt_component_publish_telemetry();
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            s_state.state = MQTT_STATE_DISCONNECTED;
            
            if (s_state.telemetry_task != NULL) {
                vTaskDelete(s_state.telemetry_task);
                s_state.telemetry_task = NULL;
            }
            
            // Start reconnection with backoff (if Wi-Fi has IP)
            if (wifi_component_has_ip()) {
                s_state.state = MQTT_STATE_RECONNECTING;
                ESP_LOGI(TAG, "Will reconnect in %lu ms", s_state.reconnect_backoff_ms);
                s_state.reconnect_backoff_ms = MIN(s_state.reconnect_backoff_ms * 2, RECONNECT_BACKOFF_MAX_MS);
            }
            break;
            
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            {
                // Queue command for async processing
                mqtt_cmd_t cmd = {0};
                int topic_len = MIN(event->topic_len, sizeof(cmd.topic) - 1);
                int payload_len = MIN(event->data_len, sizeof(cmd.payload) - 1);
                
                memcpy(cmd.topic, event->topic, topic_len);
                cmd.topic[topic_len] = '\0';
                
                memcpy(cmd.payload, event->data, payload_len);
                cmd.payload[payload_len] = '\0';
                
                if (xQueueSend(s_state.command_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
                    ESP_LOGW(TAG, "Command queue full, dropping command");
                }
            }
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            break;
            
        default:
            ESP_LOGD(TAG, "Other MQTT event: %d", event->event_id);
            break;
    }
}


static void telemetry_task(void *arg) {
    ESP_LOGI(TAG, "Telemetry task started");
    
    while (s_state.state == MQTT_STATE_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));
        
        if (s_state.state == MQTT_STATE_CONNECTED) {
            mqtt_component_publish_telemetry();
        }
    }
    
    ESP_LOGI(TAG, "Telemetry task ended");
    s_state.telemetry_task = NULL;
    vTaskDelete(NULL);
}


static void handle_led_command(cJSON *payload) {
    if (payload == NULL) {
        return;
    }
    
    cJSON *action = cJSON_GetObjectItem(payload, "action");
    if (action == NULL || action->type != cJSON_String) {
        ESP_LOGW(TAG, "Invalid LED command: missing 'action'");
        return;
    }
    
    const char *action_str = action->valuestring;
    ESP_LOGI(TAG, "LED command: %s", action_str);
    
    if (strcmp(action_str, "set_color") == 0) {
        cJSON *r = cJSON_GetObjectItem(payload, "r");
        cJSON *g = cJSON_GetObjectItem(payload, "g");
        cJSON *b = cJSON_GetObjectItem(payload, "b");
        
        if (r && g && b && r->type == cJSON_Number && g->type == cJSON_Number && b->type == cJSON_Number) {
            uint8_t red = (uint8_t)r->valueint;
            uint8_t green = (uint8_t)g->valueint;
            uint8_t blue = (uint8_t)b->valueint;
            
            ESP_LOGI(TAG, "Setting LED color: R=%d G=%d B=%d", red, green, blue);
            esp_err_t ret = wifi_component_set_led_override(red, green, blue, MQTT_LED_OVERRIDE_MS);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to set LED color: %s", esp_err_to_name(ret));
            }
        } else {
            ESP_LOGW(TAG, "Invalid color values in LED command");
        }
    } else if (strcmp(action_str, "on") == 0) {
        ESP_LOGI(TAG, "LED ON");
        (void)wifi_component_set_led_override(0, 255, 0, MQTT_LED_OVERRIDE_MS);
    } else if (strcmp(action_str, "off") == 0) {
        ESP_LOGI(TAG, "LED OFF");
        (void)wifi_component_set_led_override(0, 0, 0, MQTT_LED_OVERRIDE_MS);
    } else {* During the override window, Wi-Fi status LED updates are paused and the
 * provided color is shown. After timeout, normal Wi-Fi LED behavior resumes.
        ESP_LOGW(TAG, "Unknown LED action: %s", action_str);
    }
}

static void command_task(void *arg) {
    ESP_LOGI(TAG, "Command processing task started");
    mqtt_cmd_t cmd;
    
    while (1) {
        if (xQueueReceive(s_state.command_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ESP_LOGI(TAG, "Processing command from topic: %s", cmd.topic);
            
            // Parse JSON payload
            cJSON *root = cJSON_Parse(cmd.payload);
            if (root == NULL) {
                ESP_LOGW(TAG, "Failed to parse JSON payload: %s", cmd.payload);
                continue;
            }
            
            // Route command based on topic
            if (strcmp(cmd.topic, MQTT_CMD_TOPIC) == 0) {
                handle_led_command(root);
            } else {
                ESP_LOGW(TAG, "Unknown command topic: %s", cmd.topic);
            }
            
            cJSON_Delete(root);
        }
    }
    
    vTaskDelete(NULL);
}
