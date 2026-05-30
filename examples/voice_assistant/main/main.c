/*
 * Voice Assistant — Main Entry Point
 *
 * Initializes BSP (power, NVS, codec, display) then starts
 * the assistant (Wi-Fi → WebSocket → audio streaming → UI).
 */

#include "esp_log.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "esp-bsp.h"
#include "bsp_board_extra.h"
#include "assistant.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== reTerminal D1001 Voice Assistant ===");

    /* ── Board init ─────────────────────────────────────────────── */

    bsp_power_init();

    /* NVS for Wi-Fi credentials storage */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* SPIFFS (for future TTS audio files, etc.) */
    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_LOGI(TAG, "SPIFFS mounted");

    /* Audio codec (mic ES7210 + speaker ES8311) */
    ESP_ERROR_CHECK(bsp_extra_codec_init());
    ESP_LOGI(TAG, "Audio codec initialized");

    /* ── Display ────────────────────────────────────────────────── */

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
        }
    };
    lv_display_t *display = bsp_display_start_with_config(&cfg);

    /* Dim LEDs */
    bsp_rgb_led_duty_set(0, 0);
    bsp_rgb_led_duty_set(1, 0);
    bsp_rgb_led_duty_set(2, 0);

    /* ── Assistant UI + engine ──────────────────────────────────── */

    assistant_ui_create(display);
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "Display ready");

    assistant_init();
    ESP_LOGI(TAG, "Assistant initialized");

    /* ── Connect ────────────────────────────────────────────────── */

    ESP_LOGI(TAG, "Connecting to network and NUC server...");
    esp_err_t ret = assistant_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start assistant (check Wi-Fi config)");
    }

    ESP_LOGI(TAG, "Main init complete — press 'Press to Talk' on screen");
}
