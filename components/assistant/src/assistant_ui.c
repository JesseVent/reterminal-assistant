/*
 * Voice Assistant — LVGL Status UI
 *
 * Full-screen 800x1280 display with:
 *   - Status text (disconnected / connecting / connected / listening / speaking)
 *   - Audio waveform bars (6 animated bars)
 *   - Press-to-talk touch button
 */

#include "esp_log.h"
#include "esp_random.h"
#include "lvgl.h"
#include "assistant.h"

static const char *TAG = "assistant_ui";

/* Widget handles */
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_wave_bars[6] = {0};
static lv_obj_t *s_btn_talk = NULL;
static lv_obj_t *s_btn_label = NULL;
static lv_timer_t *s_wave_timer = NULL;
static int s_audio_level = 0;
static bool s_is_listening = false;

/* Colors — initialized at runtime for LVGL 8 compatibility */
static lv_color_t COLOR_BG, COLOR_DISCONNECTED, COLOR_CONNECTING;
static lv_color_t COLOR_CONNECTED, COLOR_LISTENING, COLOR_SPEAKING;

static void init_colors(void)
{
    COLOR_BG         = lv_color_hex(0x000000);
    COLOR_DISCONNECTED = lv_color_hex(0x666666);
    COLOR_CONNECTING  = lv_color_hex(0xFFAA00);
    COLOR_CONNECTED   = lv_color_hex(0x00CC66);
    COLOR_LISTENING   = lv_color_hex(0x00AAFF);
    COLOR_SPEAKING    = lv_color_hex(0xFF6600);
}

static const char *state_to_text(assistant_state_t state)
{
    switch (state) {
    case ASSISTANT_STATE_DISCONNECTED: return "Disconnected";
    case ASSISTANT_STATE_CONNECTING:   return "Connecting...";
    case ASSISTANT_STATE_CONNECTED:    return "Connected";
    case ASSISTANT_STATE_LISTENING:    return "Listening...";
    case ASSISTANT_STATE_SPEAKING:     return "Speaking...";
    default:                           return "Unknown";
    }
}

static lv_color_t state_to_color(assistant_state_t state)
{
    switch (state) {
    case ASSISTANT_STATE_DISCONNECTED: return COLOR_DISCONNECTED;
    case ASSISTANT_STATE_CONNECTING:   return COLOR_CONNECTING;
    case ASSISTANT_STATE_CONNECTED:    return COLOR_CONNECTED;
    case ASSISTANT_STATE_LISTENING:    return COLOR_LISTENING;
    case ASSISTANT_STATE_SPEAKING:     return COLOR_SPEAKING;
    default:                           return COLOR_DISCONNECTED;
    }
}

static void wave_timer_cb(lv_timer_t *timer)
{
    if (!s_is_listening) return;

    /* Generate pseudo-random bar heights based on audio level */
    int base = s_audio_level;
    for (int i = 0; i < 6; i++) {
        int h = base + (esp_random() % 40) - 20;
        if (h < 5) h = 5;
        if (h > 100) h = 100;
        lv_bar_set_value(s_wave_bars[i], h, LV_ANIM_ON);
    }
}

static void btn_talk_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_is_listening = true;
        assistant_start_listening();
        lv_label_set_text(s_btn_label, "Release to Stop");
    } else if (code == LV_EVENT_RELEASED) {
        s_is_listening = false;
        assistant_stop_listening();
        lv_label_set_text(s_btn_label, "Press to Talk");

        /* Reset bars */
        for (int i = 0; i < 6; i++) {
            lv_bar_set_value(s_wave_bars[i], 0, LV_ANIM_ON);
        }
    }
}

lv_obj_t *assistant_ui_create(lv_disp_t *disp)
{
    init_colors();

    s_screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_screen, 800, 1280);
    lv_obj_center(s_screen);
    lv_obj_set_style_bg_color(s_screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_screen, 0, LV_PART_MAIN);

    /* Status label — top center */
    s_status_label = lv_label_create(s_screen);
    lv_label_set_text(s_status_label, "Disconnected");
    lv_obj_set_style_text_color(s_status_label, COLOR_DISCONNECTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 100);

    /* Waveform bars — center */
    int bar_w = 40;
    int bar_gap = 20;
    int total_w = 6 * bar_w + 5 * bar_gap;
    int start_x = (800 - total_w) / 2;

    for (int i = 0; i < 6; i++) {
        s_wave_bars[i] = lv_bar_create(s_screen);
        lv_obj_set_size(s_wave_bars[i], bar_w, 200);
        lv_obj_align(s_wave_bars[i], LV_ALIGN_TOP_LEFT, start_x + i * (bar_w + bar_gap), 400);
        lv_bar_set_range(s_wave_bars[i], 0, 100);
        lv_bar_set_value(s_wave_bars[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_wave_bars[i], COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_wave_bars[i], COLOR_LISTENING, LV_PART_INDICATOR);
    }

    /* Wave animation timer */
    s_wave_timer = lv_timer_create(wave_timer_cb, 60, NULL);

    /* Press-to-talk button — bottom center */
    s_btn_talk = lv_btn_create(s_screen);
    lv_obj_set_size(s_btn_talk, 400, 100);
    lv_obj_align(s_btn_talk, LV_ALIGN_BOTTOM_MID, 0, -120);
    lv_obj_set_style_bg_color(s_btn_talk, COLOR_CONNECTED, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_btn_talk, COLOR_LISTENING, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_btn_talk, 20, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_talk, btn_talk_cb, LV_EVENT_ALL, NULL);

    s_btn_label = lv_label_create(s_btn_talk);
    lv_label_set_text(s_btn_label, "Press to Talk");
    lv_obj_set_style_text_font(s_btn_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_btn_label, COLOR_BG, LV_PART_MAIN);
    lv_obj_center(s_btn_label);

    ESP_LOGI(TAG, "UI created");
    return s_screen;
}

void assistant_ui_update_state(assistant_state_t state)
{
    if (s_status_label == NULL) return;

    const char *text = state_to_text(state);
    lv_color_t color = state_to_color(state);

    lv_label_set_text(s_status_label, text);
    lv_obj_set_style_text_color(s_status_label, color, LV_PART_MAIN);

    /* Update bar indicator color to match state */
    for (int i = 0; i < 6; i++) {
        if (s_wave_bars[i]) {
            lv_obj_set_style_bg_color(s_wave_bars[i], color, LV_PART_INDICATOR);
        }
    }

    /* Enable/disable talk button based on connection */
    if (s_btn_talk) {
        bool connected = (state >= ASSISTANT_STATE_CONNECTED);
        lv_obj_add_flag(s_btn_talk, connected ? LV_OBJ_FLAG_CLICKABLE : LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_talk, connected ? LV_OBJ_FLAG_HIDDEN : LV_OBJ_FLAG_CLICKABLE);
    }
}

void assistant_ui_update_audio_level(int level)
{
    s_audio_level = level;
}
