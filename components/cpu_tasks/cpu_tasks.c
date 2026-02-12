#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h" 
#include "cpu_tasks.h"

static const char *TAG = "CPU_LOAD";

// Burns CPU for 1 second and then sleeps for 3 seconds, repeatedly
void cpu_load_task(void *pvParameters) {
    int core_id = (int)pvParameters;
    
    while (1) {
        
        int64_t start_time = esp_timer_get_time();
        
        while ((esp_timer_get_time() - start_time) < 1000000) { //microseconds
            __asm__("nop");
        }

        ESP_LOGI(TAG, "Core %d: Finished 1s Heavy Load", core_id);

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void start_cpu_tasks(void) {
    xTaskCreatePinnedToCore(cpu_load_task, "Load_Core0", 4096, (void*)0, 1, NULL, 0);
    xTaskCreatePinnedToCore(cpu_load_task, "Load_Core1", 4096, (void*)1, 1, NULL, 1);
    
    ESP_LOGI(TAG, "Started CPU Load tasks on Core 0 and Core 1");
}