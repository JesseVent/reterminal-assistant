/*
 * Voice Assistant — State Machine Coordinator
 *
 * Wires together Wi-Fi, WebSocket, audio streaming, and UI.
 * Manages state transitions and threading.
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "lvgl.h"

#include "assistant.h"
#include "bsp_board_extra.h"

/* Pull in function declarations from sibling .c files */
extern esp_err_t assistant_wifi_init(void);
extern esp_err_t assistant_wifi_connect(void);
extern esp_err_t assistant_ws_init(RingbufHandle_t playback_rb);
extern esp_err_t assistant_ws_start(void);
extern esp_err_t assistant_ws_stop(void);
extern bool assistant_ws_is_connected(void);
extern RingbufHandle_t assistant_audio_init(void);
extern esp_err_t assistant_audio_start(void);
extern esp_err_t assistant_audio_stop(void);
extern void assistant_audio_set_listening(bool listening);
extern lv_obj_t *assistant_ui_create(lv_disp_t *disp);
extern void assistant_ui_update_state(assistant_state_t state);
extern void assistant_ui_update_audio_level(int level);

static const char *TAG = "assistant";

static assistant_state_t s_state = ASSISTANT_STATE_DISCONNECTED;
static SemaphoreHandle_t s_state_mutex = NULL;
static assistant_state_cb_t s_state_cb = NULL;
static void *s_state_cb_ctx = NULL;

/* ── State management ────────────────────────────────────────────── */

static void set_state(assistant_state_t new_state)
{
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (s_state == new_state) {
        xSemaphoreGive(s_state_mutex);
        return;
    }
    assistant_state_t old = s_state;
    s_state = new_state;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "State: %d → %d", old, new_state);
    assistant_ui_update_state(new_state);

    if (s_state_cb) {
        s_state_cb(new_state, s_state_cb_ctx);
    }
}

assistant_state_t assistant_get_state(void)
{
    assistant_state_t st;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        st = s_state;
        xSemaphoreGive(s_state_mutex);
    } else {
        st = s_state;  /* Best effort */
    }
    return st;
}

void assistant_register_state_cb(assistant_state_cb_t cb, void *user_ctx)
{
    s_state_cb = cb;
    s_state_cb_ctx = user_ctx;
}

/* ── WebSocket lifecycle callbacks (called from WS event handler) ── */

void assistant_ws_on_connected(void)
{
    set_state(ASSISTANT_STATE_CONNECTED);
}

void assistant_ws_on_disconnected(void)
{
    set_state(ASSISTANT_STATE_DISCONNECTED);
    assistant_audio_set_listening(false);
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t assistant_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_FAIL;
    }

    /* Init audio (creates ring buffer) */
    RingbufHandle_t rb = assistant_audio_init();
    if (rb == NULL) {
        return ESP_FAIL;
    }

    /* Init Wi-Fi */
    ESP_ERROR_CHECK(assistant_wifi_init());

    /* Init WebSocket (pass ring buffer for playback) */
    ESP_ERROR_CHECK(assistant_ws_init(rb));

    ESP_LOGI(TAG, "Assistant initialized");
    return ESP_OK;
}

esp_err_t assistant_start(void)
{
    set_state(ASSISTANT_STATE_CONNECTING);

    /* Connect Wi-Fi */
    esp_err_t ret = assistant_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connection failed");
        set_state(ASSISTANT_STATE_DISCONNECTED);
        return ret;
    }

    /* Start audio tasks */
    ret = assistant_audio_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio tasks");
        return ret;
    }

    /* Connect WebSocket */
    ret = assistant_ws_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed");
        set_state(ASSISTANT_STATE_DISCONNECTED);
        return ret;
    }

    /* State will transition to CONNECTED via WS event callback */
    return ESP_OK;
}

esp_err_t assistant_stop(void)
{
    assistant_audio_stop();
    assistant_ws_stop();
    set_state(ASSISTANT_STATE_DISCONNECTED);
    ESP_LOGI(TAG, "Assistant stopped");
    return ESP_OK;
}

void assistant_deinit(void)
{
    assistant_stop();
    if (s_state_mutex) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }
}

esp_err_t assistant_start_listening(void)
{
    if (!assistant_ws_is_connected()) {
        ESP_LOGW(TAG, "Cannot listen: not connected");
        return ESP_ERR_INVALID_STATE;
    }
    assistant_audio_set_listening(true);
    set_state(ASSISTANT_STATE_LISTENING);
    return ESP_OK;
}

esp_err_t assistant_stop_listening(void)
{
    assistant_audio_set_listening(false);
    /* In loopback mode we go back to connected.
     * In full pipeline, we'd transition to SPEAKING here. */
    if (assistant_ws_is_connected()) {
        set_state(ASSISTANT_STATE_CONNECTED);
    }
    return ESP_OK;
}
