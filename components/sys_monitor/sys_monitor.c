#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sys_monitor.h"

static const char *TAG = "SYS_MONITOR";

void system_monitor_task(void *pvParameters) {
    #if configUSE_TRACE_FACILITY && configUSE_STATS_FORMATTING_FUNCTIONS
    char *stats_buffer = NULL;
    #endif

    while (1)
    {
        ESP_LOGI(TAG, "-------------------------------");
        ESP_LOGI(TAG, "[RAM] Free: %lu bytes | Min Free: %lu bytes", 
                 esp_get_free_heap_size(), 
                 esp_get_minimum_free_heap_size());

        #if configUSE_TRACE_FACILITY && configUSE_STATS_FORMATTING_FUNCTIONS
            stats_buffer = malloc(4096);
            if (stats_buffer != NULL) {
                // Returns: Name, State, Priority, Stack, Task Number
                vTaskList(stats_buffer);
                ESP_LOGI(TAG, "Task Name\tStatus\tPrio\tStack\tTask#");
                printf("%s\n", stats_buffer);
                free(stats_buffer);
            } else {
                ESP_LOGE(TAG, "Failed to allocate memory for vTaskList");
            }
        #else
            ESP_LOGW(TAG, "vTaskList is disabled. Enable 'Trace facility' & 'Stats formatting' in menuconfig.");
        #endif

        ESP_LOGI(TAG, "-------------------------------");
        vTaskDelay(pdMS_TO_TICKS(5000)); 
    } 
}

void start_system_monitor(void) {
    xTaskCreatePinnedToCore(system_monitor_task, "sys_monitor", 4096, NULL, 1, NULL, 0);
}