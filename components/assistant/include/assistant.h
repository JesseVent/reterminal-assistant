/*
 * Voice Assistant — Public API
 *
 * Provides Wi-Fi connectivity, WebSocket audio streaming to a NUC server,
 * I2S audio capture/playback, and an LVGL status UI.
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************************
 * State machine
 **************************************************************************************************/

typedef enum {
    ASSISTANT_STATE_DISCONNECTED,   // No Wi-Fi or no WebSocket
    ASSISTANT_STATE_CONNECTING,     // Wi-Fi connecting or WS connecting
    ASSISTANT_STATE_CONNECTED,      // WS connected, idle — ready to talk
    ASSISTANT_STATE_LISTENING,      // Mic streaming to NUC
    ASSISTANT_STATE_SPEAKING,       // Speaker playing from NUC
} assistant_state_t;

typedef void (*assistant_state_cb_t)(assistant_state_t state, void *user_ctx);

/**************************************************************************************************
 * Lifecycle
 **************************************************************************************************/

/**
 * @brief Initialize all subsystems (Wi-Fi, WebSocket, audio codec, UI).
 *
 * Does NOT connect yet. Call assistant_start() to begin connection.
 *
 * @return ESP_OK on success
 */
esp_err_t assistant_init(void);

/**
 * @brief Connect to Wi-Fi and the NUC WebSocket server.
 *
 * Blocks until Wi-Fi is connected (with timeout). WebSocket connects
 * asynchronously — register a state callback to know when it's ready.
 *
 * @return ESP_OK on success
 */
esp_err_t assistant_start(void);

/**
 * @brief Disconnect and stop all streaming.
 *
 * @return ESP_OK on success
 */
esp_err_t assistant_stop(void);

/**
 * @brief Free all resources.
 */
void assistant_deinit(void);

/**************************************************************************************************
 * State
 **************************************************************************************************/

/**
 * @brief Get current assistant state (thread-safe).
 */
assistant_state_t assistant_get_state(void);

/**
 * @brief Register a callback for state changes.
 *
 * Called from the LVGL task context or event task.
 */
void assistant_register_state_cb(assistant_state_cb_t cb, void *user_ctx);

/**************************************************************************************************
 * Audio control
 **************************************************************************************************/

/**
 * @brief Start streaming microphone audio to the NUC.
 *
 * State transitions to ASSISTANT_STATE_LISTENING.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t assistant_start_listening(void);

/**
 * @brief Stop streaming microphone audio.
 *
 * State transitions back to ASSISTANT_STATE_CONNECTED.
 *
 * @return ESP_OK on success
 */
esp_err_t assistant_stop_listening(void);

/**************************************************************************************************
 * LVGL UI
 **************************************************************************************************/

/**
 * @brief Create the assistant status screen on the given display.
 *
 * Creates a full-screen 800x1280 panel with:
 *  - Status text (disconnected / connecting / connected / listening / speaking)
 *  - Audio level waveform bars
 *  - Press-to-talk touch button
 *
 * @param disp  LVGL display (from bsp_display_start_with_config)
 * @return      Created screen object
 */
lv_obj_t *assistant_ui_create(lv_display_t *disp);

/**
 * @brief Update UI to reflect current state.
 *
 * Called internally by state machine; also safe to call manually.
 */
void assistant_ui_update_state(assistant_state_t state);

/**
 * @brief Update the audio level waveform (0-100).
 *
 * Call from the record task to visualize mic input.
 */
void assistant_ui_update_audio_level(int level);

#ifdef __cplusplus
}
#endif
