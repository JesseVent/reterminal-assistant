/*
 * Voice Assistant — WebSocket Client
 *
 * Manages the WebSocket connection to the NUC server.
 * Sends binary audio frames (PCM) upstream, receives binary audio
 * and text (JSON) frames downstream.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

static const char *TAG = "assistant_ws";

static esp_websocket_client_handle_t s_ws_client = NULL;
static RingbufHandle_t s_playback_rb = NULL;
static bool s_is_connected = false;

/* Forward-declared: defined in assistant.c */
extern void assistant_ws_on_connected(void);
extern void assistant_ws_on_disconnected(void);
extern void assistant_ws_on_state_changed(bool connected);

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_is_connected = true;
        assistant_ws_on_connected();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        s_is_connected = false;
        assistant_ws_on_disconnected();
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == WS_TRANSPORT_OPCODES_BINARY && data->data_len > 0) {
            /* Incoming audio from NUC — push into playback ring buffer */
            if (s_playback_rb != NULL) {
                BaseType_t ret = xRingbufferSend(s_playback_rb,
                    data->data_ptr, data->data_len, pdMS_TO_TICKS(50));
                if (ret != pdTRUE) {
                    ESP_LOGW(TAG, "Playback buffer full, dropped %d bytes", data->data_len);
                }
            }
        } else if (data->op_code == WS_TRANSPORT_OPCODES_TEXT && data->data_len > 0) {
            ESP_LOGI(TAG, "Text frame: %.*s", data->data_len, (char *)data->data_ptr);
            /* Future: parse JSON metadata, update UI */
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

esp_err_t assistant_ws_init(RingbufHandle_t playback_rb)
{
    s_playback_rb = playback_rb;

    esp_websocket_client_config_t ws_cfg = {
        .uri = CONFIG_ASSISTANT_WS_URI,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .buffer_size = 8192,
        .ping_interval_sec = 10,
        .auto_reconnect = true,
    };

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    if (s_ws_client == NULL) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_websocket_register_events(
        s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL));

    ESP_LOGI(TAG, "WebSocket client initialized (target: %s)", CONFIG_ASSISTANT_WS_URI);
    return ESP_OK;
}

esp_err_t assistant_ws_start(void)
{
    if (s_ws_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Starting WebSocket connection...");
    return esp_websocket_client_start(s_ws_client);
}

esp_err_t assistant_ws_stop(void)
{
    if (s_ws_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_is_connected = false;
    return esp_websocket_client_destroy(s_ws_client);
    s_ws_client = NULL;
}

bool assistant_ws_is_connected(void)
{
    return s_is_connected && esp_websocket_client_is_connected(s_ws_client);
}

esp_err_t assistant_ws_send_audio(const uint8_t *data, size_t len)
{
    if (!assistant_ws_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_bin(s_ws_client, data, len, pdMS_TO_TICKS(100));
    if (sent < 0) {
        ESP_LOGW(TAG, "Failed to send audio (%d bytes)", len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t assistant_ws_send_text(const char *json)
{
    if (!assistant_ws_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_text(s_ws_client, json, strlen(json), pdMS_TO_TICKS(100));
    if (sent < 0) {
        ESP_LOGW(TAG, "Failed to send text");
        return ESP_FAIL;
    }
    return ESP_OK;
}
