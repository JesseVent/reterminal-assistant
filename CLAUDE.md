# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

reTerminal D1001 SDK by Seeed Studio — an embedded firmware project for the reTerminal D1001 IoT device based on the **ESP32-P4** MCU. Built on **ESP-IDF v5.4.2** (compatible with v5.3+). The device features an 800×1280 MIPI DSI LCD (JD9365), capacitive touch (GSL3670), camera (SC2356), audio codecs (ES8311 DAC / ES7210 ADC), IMU (LSM6DS3), RTC (PCF8563), IO expander (PCA9535), battery management, Wi-Fi (via ESP-Hosted over SDIO to ESP32-C6 slave), LTE modem (USB), and LoRa (SPI).

## Build System

Uses ESP-IDF's `idf.py` build system (CMake-based). **Target is always `esp32p4`.**

```bash
# Set target (required once per project)
idf.py set-target esp32p4

# Build
idf.py build

# Flash and monitor (replace /dev/ttyACM0 with actual serial port)
idf.py --port /dev/ttyACM0 flash monitor

# Combined build + flash + monitor
idf.py --port /dev/ttyACM0 build flash monitor

# Erase flash
idf.py --port /dev/ttyACM0 erase-flash
```

Exit serial monitor with `Ctrl+]`.

## Repository Structure

- **`components/`** — Reusable ESP-IDF components (board support package + drivers)
- **`examples/`** — Full applications (factory firmware demo)
- **`driver_examples/`** — Minimal peripheral-specific examples (I2S audio codecs)
- **`firmware/`** — Pre-built factory firmware `.bin` files

### Component Dependency Graph

```
esp32_p4_re_terminal_d1001 (BSP — main board support)
  ├── esp_lcd_jd9365_8        (8" LCD panel driver, MIPI DSI)
  ├── esp_lcd_touch_gsl3670   (capacitive touch controller)
  │   └── esp_io_expander_pca9535 (I2C IO expander)
  ├── esp_cam_sensor           (camera sensor driver, SC2356)
  ├── bsp_extra                (additional board utilities: codec init, SD card)
  │   └── esp32_p4_re_terminal_d1001
  ├── esp-brookesia            (HMI/UI framework, LVGL-based, v0.4.2 — uses LVGL 8.x)
  └── iot_usbh_modem           (USB Host LTE modem/PPP driver)
```

Components reference each other via `path:` in `idf_component.yml`, meaning they are local path dependencies, not fetched from a registry.

### BSP (`components/esp32_p4_re_terminal_d1001/`)

The core board support package. Key headers in `include/`:
- `esp32_p4_re_terminal_d1001.h` — Master BSP API (I2C buses, audio, display, SD card, SPIFFS, battery, power, LEDs)
- `display.h` — LCD panel config (resolution, MIPI DSI parameters) and low-level display API
- `config.h` — Graphic library toggle (`BSP_CONFIG_NO_GRAPHIC_LIB`)
- `bsp_lsm6ds3.h` — IMU (accelerometer + gyroscope)
- `bsp_pcf8563.h` — RTC

I2C bus layout:
- **I2C_0** (GPIO 38/37) — touch controller, camera, light sensor
- **I2C_1** (GPIO 21/20) — ADC (ES7210), DAC (ES8311), RTC (PCF8563), IO expander (PCA9535)

### Factory Firmware (`examples/factory_firmware/`)

A full demo app using esp-brookesia (phone-style UI). Written in C++ with LVGL. Apps live in `components/apps/` and include: Calculator, Camera, Game 2048, Image Display, LCD Test, Music Player, Power Off, Recorder, Sensor, Settings, Touch Test, Video Player.

Partition table (`partitions.csv`): factory app (9MB) + SPIFFS storage (4MB).

The `main/` entry point (`main.cpp`) initializes power, NVS, SPIFFS, SD card, audio codec, display with LVGL, then launches the esp-brookesia phone UI with all app widgets.

## Configuration

`sdkconfig.defaults` files contain pre-set project configurations. Key settings to be aware of:

- **PSRAM** enabled (`CONFIG_SPIRAM=y`) with 200MHz speed, used for LVGL frame buffers (`buff_spiram: true`)
- **Dual frame buffers** with **direct mode** for tear-free display (`BSP_LCD_DPI_BUFFER_NUMS=2`, `BSP_DISPLAY_LVGL_DIRECT_MODE=y`)
- **Wi-Fi** via ESP-Hosted SDIO to ESP32-C6 slave (`CONFIG_SLAVE_IDF_TARGET_ESP32C6=y`)
- **Camera** SC2356 with MIPI interface, ISP pipeline enabled
- **PPP/LTE** support enabled (`CONFIG_LWIP_PPP_SUPPORT=y`)
- **Console** on USB Serial JTAG (not UART)

Kconfig options for the BSP are in `components/esp32_p4_re_terminal_d1001/Kconfig` (SD card, SPIFFS, display settings). The factory firmware has its own `Kconfig.projbuild` for app-level options.

## Code Conventions

- C for drivers/BSP, C++ for application-level code (factory firmware apps)
- ESP-IDF error checking pattern: `ESP_ERROR_CHECK(bsp_*(...))` or `esp_err_t` return codes
- LVGL APIs require mutex: `bsp_display_lock()` / `bsp_display_unlock()` around `lv_*` calls
- Pin definitions use `GPIO_NUM_*` for direct pins and `(1ULL << N)` for IO-expander pins (prefixed `EXP_PIN_NUM_*` in comments)
- SPDX license headers on Espressif-originated files (Apache-2.0)

## Adding New Components

New ESP-IDF components go in `components/`. Each needs:
- `CMakeLists.txt` with `idf_component_register()`
- `idf_component.yml` for dependency resolution
- Reference local sibling components with `path: ../component_name`
