/*
 * Voice Assistant — Audio Streaming
 *
 * Two FreeRTOS tasks:
 *   1. Record task: I2S mic read → WebSocket send
 *   2. Playback task: Ring buffer → I2S speaker write
 *
 * Uses BSP Extra codec functions for audio I/O.
 * Audio format: 16kHz, 16-bit, mono PCM.
 */

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "bsp_board_extra.h"

static const char *TAG = "assistant_audio";

#define CHUNK_SIZE  CONFIG_ASSISTANT_AUDIO_CHUNK_SIZE   /* 4096 default */
#define RB_SIZE     CONFIG_ASSISTANT_RING_BUF_SIZE      /* 65536 default */

static TaskHandle_t s_record_task = NULL;
static TaskHandle_t s_playback_task = NULL;
static volatile bool s_is_listening = false;
static volatile bool s_tasks_running = false;

/* Shared with assistant_ws.c */
static RingbufHandle_t s_playback_rb = NULL;

/* Forward-declared from assistant_ws.c */
extern esp_err_t assistant_ws_send_audio(const uint8_t *data, size_t len);

/* Forward-declared from assistant_ui.c */
extern void assistant_ui_update_audio_level(int level);

/**
 * Compute RMS of 16-bit samples to get a rough audio level (0-100).
 */
static int compute_audio_level(const int16_t *samples, size_t num_samples)
{
    if (num_samples == 0) return 0;
    int64_t sum = 0;
    for (size_t i = 0; i < num_samples; i++) {
        int32_t s = samples[i];
        sum += s * s;
    }
    double rms = sqrt((double)sum / num_samples);
    /* Map RMS (0..32767) to 0..100 with log-ish curve for visual appeal */
    int level = (int)(rms / 327.67);
    if (level > 100) level = 100;
    return level;
}

/**
 * Record task: read from mic, send to WebSocket.
 */
static void audio_record_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate record buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Record task started");

    while (s_tasks_running) {
        if (!s_is_listening) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t ret = bsp_extra_i2s_read(buf, CHUNK_SIZE, &bytes_read, 100);
        if (ret != ESP_OK || bytes_read == 0) {
            continue;
        }

        /* Send audio to WebSocket (NUC) */
        assistant_ws_send_audio(buf, bytes_read);

        /* Update UI waveform */
        int level = compute_audio_level((int16_t *)buf, bytes_read / 2);
        assistant_ui_update_audio_level(level);
    }

    free(buf);
    ESP_LOGI(TAG, "Record task stopped");
    vTaskDelete(NULL);
}

/**
 * Playback task: receive from ring buffer, write to speaker.
 */
static void audio_playback_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate playback buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Playback task started");

    while (s_tasks_running) {
        size_t bytes_avail = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceive(s_playback_rb, &bytes_avail, pdMS_TO_TICKS(100));
        if (data == NULL || bytes_avail == 0) {
            continue;
        }

        /* Write to speaker in CHUNK_SIZE chunks */
        size_t offset = 0;
        while (offset < bytes_avail) {
            size_t to_write = bytes_avail - offset;
            if (to_write > CHUNK_SIZE) to_write = CHUNK_SIZE;
            memcpy(buf, data + offset, to_write);

            size_t bytes_written = 0;
            bsp_extra_i2s_write(buf, to_write, &bytes_written, 100);
            offset += to_write;
        }

        /* Return ring buffer item */
        vRingbufferReturnItem(s_playback_rb, data);
    }

    free(buf);
    ESP_LOGI(TAG, "Playback task stopped");
    vTaskDelete(NULL);
}

RingbufHandle_t assistant_audio_init(void)
{
    /* Create ring buffer for incoming audio (NUC → speaker) */
    s_playback_rb = xRingbufferCreate(RB_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (s_playback_rb == NULL) {
        ESP_LOGE(TAG, "Failed to create playback ring buffer (%d bytes)", RB_SIZE);
        return NULL;
    }

    ESP_LOGI(TAG, "Audio subsystem initialized (chunk=%d, ringbuf=%d)", CHUNK_SIZE, RB_SIZE);
    return s_playback_rb;
}

esp_err_t assistant_audio_start(void)
{
    if (s_tasks_running) {
        return ESP_OK;  /* Already running */
    }
    s_tasks_running = true;

    BaseType_t ret;
    ret = xTaskCreatePinnedToCore(audio_record_task, "audio_rec",
        6 * 1024, NULL, 5, &s_record_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create record task");
        s_tasks_running = false;
        return ESP_FAIL;
    }

    ret = xTaskCreatePinnedToCore(audio_playback_task, "audio_play",
        6 * 1024, NULL, 4, &s_playback_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        s_tasks_running = false;
        vTaskDelete(s_record_task);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio tasks started");
    return ESP_OK;
}

esp_err_t assistant_audio_stop(void)
{
    s_tasks_running = false;
    s_is_listening = false;

    /* Tasks will exit on their own via the s_tasks_running flag */
    ESP_LOGI(TAG, "Audio tasks stopping...");
    return ESP_OK;
}

void assistant_audio_set_listening(bool listening)
{
    s_is_listening = listening;
    if (!listening) {
        assistant_ui_update_audio_level(0);
    }
    ESP_LOGI(TAG, "Listening: %s", listening ? "ON" : "OFF");
}
