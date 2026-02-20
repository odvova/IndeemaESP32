#ifndef HTTP_CONFIG_H
#define HTTP_CONFIG_H

#include "esp_err.h"

/**
 * @brief Initialize and start the HTTP configuration server
 * 
 * Starts an HTTP server on port 80 with endpoints for:
 * - GET / - Configuration form
 * - POST /wifi - Update Wi-Fi credentials
 * - POST /mode - Switch Wi-Fi mode
 * 
 * The HTTP server requires wifi_component to be initialized.
 * 
 * @return ESP_OK if server started successfully
 */
esp_err_t http_config_start(void);

/**
 * @brief Stop the HTTP configuration server
 */
void http_config_stop(void);

#endif // HTTP_CONFIG_H
