#include "http_config.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "wifi_component.h"

static const char *TAG = "HTTP_CONFIG";

#define HTTP_BUF_SIZE 256
#define SSID_MAX_LEN 32
#define PASS_MAX_LEN 64

static httpd_handle_t s_http_server = NULL;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *dst, size_t dst_size, const char *src)
{
    size_t write = 0;
    for (size_t read = 0; src[read] != '\0' && write + 1 < dst_size; read++) {
        if (src[read] == '+') {
            dst[write++] = ' ';
        } else if (src[read] == '%' && src[read + 1] != '\0' && src[read + 2] != '\0') {
            int hi = hex_value(src[read + 1]);
            int lo = hex_value(src[read + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[write++] = (char)((hi << 4) | lo);
                read += 2;
            } else {
                dst[write++] = src[read];
            }
        } else {
            dst[write++] = src[read];
        }
    }
    dst[write] = '\0';
}

static void parse_form_value(const char *body, const char *key, char *out, size_t out_size)
{
    out[0] = '\0';
    size_t key_len = strlen(key);
    const char *cursor = body;

    while (*cursor != '\0') {
        const char *amp = strchr(cursor, '&');
        size_t token_len = (amp != NULL) ? (size_t)(amp - cursor) : strlen(cursor);

        if (token_len > key_len + 1 && strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            char encoded[HTTP_BUF_SIZE] = {0};
            size_t value_len = token_len - key_len - 1;
            if (value_len >= sizeof(encoded)) {
                value_len = sizeof(encoded) - 1;
            }
            memcpy(encoded, cursor + key_len + 1, value_len);
            encoded[value_len] = '\0';
            url_decode(out, out_size, encoded);
            return;
        }

        if (amp == NULL) {
            break;
        }
        cursor = amp + 1;
    }
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

static esp_err_t http_root_get_handler(httpd_req_t *req)
{
    wifi_component_mode_t current_mode = wifi_component_get_mode();
    
    char html[768] = {0};
    int len = snprintf(html,
                       sizeof(html),
                       "<html><body><h2>Wi-Fi Config</h2>"
                       "<p>Current mode: <b>%s</b></p>"
                       "<form method='POST' action='/wifi'>"
                       "STA SSID: <input name='ssid' value=''><br>"
                       "STA Password: <input name='password' type='password' value=''><br>"
                       "<button type='submit'>Save and Connect STA</button>"
                       "</form>"
                       "<form method='POST' action='/mode'>"
                       "<button name='mode' value='ap' type='submit'>Switch to AP</button>"
                       "<button name='mode' value='sta' type='submit'>Switch to STA</button>"
                       "<button name='mode' value='apsta' type='submit'>Switch to AP+STA</button>"
                       "</form>"
                       "</body></html>",
                       mode_to_string(current_mode));

    if (len < 0 || len >= (int)sizeof(html)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_wifi_post_handler(httpd_req_t *req)
{
    char body[HTTP_BUF_SIZE * 2] = {0};  // Larger buffer
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        ESP_LOGW(TAG, "Invalid body len: %d", total);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_OK;
    }

    int received = httpd_req_recv(req, body, total);
    if (received <= 0) {
        ESP_LOGW(TAG, "Read error: %d", received);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read error");
        return ESP_OK;
    }
    body[received] = '\0';
    
    ESP_LOGI(TAG, "Received form: %s", body);

    char ssid[SSID_MAX_LEN + 1] = {0};
    char password[PASS_MAX_LEN + 1] = {0};
    parse_form_value(body, "ssid", ssid, sizeof(ssid));
    parse_form_value(body, "password", password, sizeof(password));

    ESP_LOGI(TAG, "Parsed SSID: '%s' (%d chars)", ssid, (int)strlen(ssid));
    ESP_LOGI(TAG, "Parsed Password: '%s' (%d chars)", password, (int)strlen(password));

    if (strlen(ssid) == 0) {
        ESP_LOGW(TAG, "SSID is empty");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID cannot be empty");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Calling wifi_component_set_sta_credentials");
    esp_err_t ret = wifi_component_set_sta_credentials(ssid, password);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Credentials rejected: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid credentials");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Credentials accepted, device now in AP+STA mode");
    return httpd_resp_sendstr(req, "Saved! Device is now in AP+STA mode. Connecting to home network...");
}

static esp_err_t http_mode_post_handler(httpd_req_t *req)
{
    char body[HTTP_BUF_SIZE] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_OK;
    }

    int received = httpd_req_recv(req, body, total);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read error");
        return ESP_OK;
    }
    body[received] = '\0';

    char mode[8] = {0};
    parse_form_value(body, "mode", mode, sizeof(mode));

    wifi_component_mode_t new_mode = WIFI_COMPONENT_MODE_AP;
    if (strcmp(mode, "sta") == 0) {
        new_mode = WIFI_COMPONENT_MODE_STA;
    } else if (strcmp(mode, "apsta") == 0) {
        new_mode = WIFI_COMPONENT_MODE_APSTA;
    }

    if (wifi_component_set_mode(new_mode) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Mode switch failed");
        return ESP_OK;
    }

    return httpd_resp_sendstr(req, "Mode updated");
}

esp_err_t http_config_start(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 8;

    esp_err_t ret = httpd_start(&s_http_server, &cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    const httpd_uri_t root_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = http_root_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t wifi_post = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = http_wifi_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t mode_post = {
        .uri = "/mode",
        .method = HTTP_POST,
        .handler = http_mode_post_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_http_server, &root_get);
    httpd_register_uri_handler(s_http_server, &wifi_post);
    httpd_register_uri_handler(s_http_server, &mode_post);
    ESP_LOGI(TAG, "HTTP config server started");
    return ESP_OK;
}

void http_config_stop(void)
{
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        ESP_LOGI(TAG, "HTTP config server stopped");
    }
}
