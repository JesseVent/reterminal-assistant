# Phase 1: Audio + WebSocket Loopback Foundation

## Context

The reTerminal D1001 (ESP32-P4) has a working BSP with audio codecs (ES7210 mic, ES8311 speaker), 800×1280 LVGL display, and Wi-Fi (via ESP-Hosted SDIO). The goal is to turn it into a voice assistant connected to a Linux NUC. Phase 1 builds the foundation: mic audio streams over Wi-Fi via WebSocket to the NUC, and the NUC echoes it back to the speaker. This validates the entire audio pipeline before adding AI (wake word, STT, LLM, TTS) in later phases.

## Architecture

```
D1001 (ESP32-P4)                          NUC (Ubuntu)
┌─────────────────────┐                  ┌──────────────┐
│  Mic (ES7210)       │  WebSocket       │              │
│  I2S RX ──► 4KB     │──► binary ──────►│  Python      │
│  chunks ──► ringbuf │   PCM audio      │  FastAPI     │
│            ──► WS TX│                  │  websockets  │
│                     │  WebSocket       │              │
│  Speaker (ES8311)   │◄── binary ──────◄│  loopback    │
│  I2S TX ◄── ringbuf │◄── PCM audio     │              │
│             ◄─ WS RX│                  └──────────────┘
│                     │
│  LVGL status UI     │  ws://NUC_IP:8080/assistant
│  (idle/listening/   │
│   speaking)         │
└─────────────────────┘
```

## Files to Create

```
components/assistant/
├── CMakeLists.txt
├── idf_component.yml
├── Kconfig
├── include/
│   └── assistant.h
├── src/
│   ├── assistant.c          # state machine + init/start
│   ├── assistant_wifi.c     # Wi-Fi STA connect
│   ├── assistant_ws.c       # WebSocket client lifecycle
│   ├── assistant_audio.c    # I2S record/play tasks + ring buffers
│   └── assistant_ui.c       # LVGL status screen

examples/voice_assistant/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   └── main.c

nuc_server/
├── requirements.txt
└── server.py
```

## Detailed Design

### 1. Component: `components/assistant/`

#### `include/assistant.h` — Public API

```c
typedef enum {
    ASSISTANT_STATE_DISCONNECTED,   // No Wi-Fi or no WebSocket
    ASSISTANT_STATE_CONNECTING,     // Wi-Fi connecting or WS connecting
    ASSISTANT_STATE_CONNECTED,      // WS connected, idle
    ASSISTANT_STATE_LISTENING,      // Mic streaming to NUC
    ASSISTANT_STATE_SPEAKING,       // Speaker playing from NUC
} assistant_state_t;

typedef void (*assistant_state_cb_t)(assistant_state_t state, void *user_ctx);

// Lifecycle
esp_err_t assistant_init(void);           // Init wifi + ws + audio (no streaming yet)
esp_err_t assistant_start(void);          // Connect wifi, then websocket
esp_err_t assistant_stop(void);           // Disconnect everything
void assistant_deinit(void);

// State
assistant_state_t assistant_get_state(void);
void assistant_register_state_cb(assistant_state_cb_t cb, void *user_ctx);

// Control
esp_err_t assistant_start_listening(void);  // Start mic → WS streaming
esp_err_t assistant_stop_listening(void);   // Stop mic streaming

// LVGL UI (creates screen on current display)
lv_obj_t *assistant_ui_create(lv_display_t *disp);
void assistant_ui_update_state(assistant_state_t state);
void assistant_ui_update_audio_level(int level);  // 0-100 for waveform
```

#### `src/assistant_wifi.c` — Wi-Fi Station

- `assistant_wifi_init()` — Initialize `esp_netif` + `esp_wifi` in STA mode
- `assistant_wifi_connect(ssid, password)` — Start connection, wait for `IP_EVENT_STA_GOT_IP`
- Uses `esp_wifi_set_mode(WIFI_MODE_STA)`, `esp_wifi_set_config()`, `esp_wifi_start()`
- SSID/password from Kconfig (`CONFIG_ASSISTANT_WIFI_SSID`, `CONFIG_ASSISTANT_WIFI_PASSWORD`)
- Event handler: on `WIFI_EVENT_STA_DISCONNECTED` → auto-reconnect with 5s backoff
- On `IP_EVENT_STA_GOT_IP` → trigger WebSocket connection

#### `src/assistant_ws.c` — WebSocket Client

- Uses `espressif/esp_websocket_client: "^1.7.0"` managed component
- Config: `.uri = CONFIG_ASSISTANT_WS_URI`, `.buffer_size = 8192`, `.auto_reconnect = true`, `.reconnect_timeout_ms = 5000`, `.ping_interval_sec = 10`
- Event handler:
  - `WEBSOCKET_EVENT_CONNECTED` → set state to CONNECTED
  - `WEBSOCKET_EVENT_DATA` + binary op_code → push into playback ring buffer
  - `WEBSOCKET_EVENT_DATA` + text op_code → parse JSON for future metadata
  - `WEBSOCKET_EVENT_CLOSED` / `WEBSOCKET_EVENT_ERROR` → set state to DISCONNECTED, retry
- Send functions:
  - `assistant_ws_send_audio(buf, len)` — `esp_websocket_client_send_bin()`
  - `assistant_ws_send_text(json)` — `esp_websocket_client_send_text()`

#### `src/assistant_audio.c` — I2S Streaming Tasks

**Key decision**: Use the BSP Extra functions (`bsp_extra_i2s_read`, `bsp_extra_i2s_write`) rather than raw I2S, since they handle codec device abstraction.

**Record task** (`audio_record_task`, priority 5, stack 6KB, PSRAM):
```
while (listening) {
    bsp_extra_i2s_read(buf, CHUNK_SIZE, &bytes_read, 100ms);
    // Downmix 4ch to mono if needed (ES7210 is 4-mic TDM)
    assistant_ws_send_audio(buf, bytes_read);
    // Also update UI audio level
    assistant_ui_update_audio_level(compute_rms(buf, bytes_read));
}
```

**Playback task** (`audio_playback_task`, priority 4, stack 6KB, PSRAM):
```
while (running) {
    // Read from ring buffer (blocking, 100ms timeout)
    bytes = ring_buffer_read(playback_rb, buf, CHUNK_SIZE, 100ms);
    if (bytes > 0) {
        bsp_extra_i2s_write(buf, bytes, &bytes_written, 100ms);
    }
}
```

**Ring buffer**: 64KB allocated in PSRAM. Written by WebSocket RX handler (from ISR context → use `ringbuf_send` from `freertos/ringbuf.h`), read by playback task. This decouples network jitter from I2S timing.

**Audio format**: 16kHz, 16-bit, mono PCM. Chunk size: 4096 bytes (128ms of audio). Data rate: ~32 KB/s.

#### `src/assistant_ui.c` — LVGL Status Screen

- Full-screen 800×1280 black background
- Top section: Status text (large font) — "Disconnected" / "Connecting..." / "Connected" / "Listening..." / "Speaking..."
- Center: Animated waveform when listening (6 bars using `lv_bar` widgets, heights driven by RMS audio level, updated every 50ms via `lv_timer`)
- Bottom: Touch button "Press to Talk" (toggles listening on/off) — large touch target
- State callback updates colors: idle=dim white, listening=green, speaking=amber

#### `src/assistant.c` — State Machine Coordinator

- `assistant_init()`: Calls wifi_init, ws_init (no connect), audio_init (codec + tasks created but suspended), ui_create
- `assistant_start()`: wifi_connect → on GOT_IP → ws_start → on WS_CONNECTED → state=CONNECTED
- State transitions:
  - CONNECTED + button press → LISTENING (resume record task)
  - LISTENING + button release → stop recording → state=CONNECTED (later: SPEAKING when TTS plays back)
  - Currently in loopback mode: LISTENING means audio echoes back immediately
- Thread safety: state changes protected by `SemaphoreHandle_t`

### 2. Example: `examples/voice_assistant/`

#### `sdkconfig.defaults`
Extends factory firmware defaults with:
```
CONFIG_IDF_TARGET="esp32p4"
# ... (inherit critical display/PSRAM settings from factory firmware)
CONFIG_ASSISTANT_WIFI_SSID="YourWiFi"
CONFIG_ASSISTANT_WIFI_PASSWORD="YourPassword"
CONFIG_ASSISTANT_WS_URI="ws://192.168.1.100:8080/assistant"
```

#### `Kconfig.projbuild`
```
config ASSISTANT_WIFI_SSID
    string "WiFi SSID"
    default "MyNetwork"

config ASSISTANT_WIFI_PASSWORD
    string "WiFi Password"
    default ""

config ASSISTANT_WS_URI
    string "WebSocket URI"
    default "ws://192.168.1.100:8080/assistant"
```

#### `main/main.c`
```c
void app_main(void) {
    bsp_power_init();
    nvs_flash_init();
    bsp_spiffs_mount();
    bsp_extra_codec_init();

    // Start display
    bsp_display_cfg_t cfg = { ... }; // Same pattern as factory firmware
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    // Init assistant
    assistant_init();
    lv_obj_t *ui = assistant_ui_create(disp);

    // Start (connects WiFi → WebSocket)
    assistant_start();
}
```

#### `CMakeLists.txt`
```cmake
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS ../../components)
project(voice_assistant VERSION 0.1.0)
```

### 3. NUC Server: `nuc_server/`

#### `server.py` — FastAPI + websockets loopback
```python
import asyncio, websockets, json

async def assistant_handler(websocket):
    print("Client connected")
    async for message in websocket:
        if isinstance(message, bytes):
            # Loopback: send audio right back
            await websocket.send(message)
        elif isinstance(message, str):
            # JSON metadata (future use)
            data = json.loads(message)
            print(f"Received: {data}")

async def main():
    async with websockets.serve(assistant_handler, "0.0.0.0", 8080):
        print("Server listening on ws://0.0.0.0:8080/assistant")
        await asyncio.Future()  # Run forever

asyncio.run(main())
```

#### `requirements.txt`
```
websockets>=12.0
```

## Existing Code to Reuse

| What | Where | Functions |
|---|---|---|
| Audio codec init | `components/bsp_extra/` | `bsp_extra_codec_init()`, `bsp_extra_i2s_read()`, `bsp_extra_i2s_write()` |
| Display init | BSP | `bsp_display_start_with_config()`, `bsp_display_backlight_on()` |
| Power init | BSP | `bsp_power_init()` |
| I2C buses | BSP | `bsp_i2c_0_init()`, `bsp_i2c_1_init()` (called internally by codec init) |
| Component registration pattern | Any component CMakeLists.txt | `idf_component_register()` with REQUIRES/PRIV_REQUIRES |
| sdkconfig pattern | `examples/factory_firmware/sdkconfig.defaults` | PSRAM, display, cache configs to copy |

## Execution Order

1. Create `nuc_server/` (server.py + requirements.txt) — test it standalone first
2. Create `components/assistant/` with all 5 source files + headers + build files
3. Create `examples/voice_assistant/` with main + build files + sdkconfig
4. Update root `.gitignore` if needed
5. Build: `cd examples/voice_assistant && idf.py set-target esp32p4 && idf.py build`
6. Test NUC server: `cd nuc_server && python3 server.py`
7. Flash D1001: `idf.py --port /dev/tty.usbmodem* flash monitor`

## Verification

1. **NUC server starts**: `python3 server.py` → prints "Server listening on ws://0.0.0.0:8080"
2. **D1001 connects**: Serial monitor shows Wi-Fi connected, got IP, WebSocket connected
3. **UI shows "Connected"**: LVGL display shows green status
4. **Press-to-talk loopback**: Touch the button → status changes to "Listening" → speak into mic → hear voice back from speaker with minimal delay
5. **Latency check**: Clap near mic → should hear it back in < 300ms
6. **Stability**: Let it run for 5 minutes, no crashes or disconnections
