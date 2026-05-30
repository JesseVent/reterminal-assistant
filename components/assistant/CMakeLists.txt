set(SRCS
    "src/assistant.c"
    "src/assistant_wifi.c"
    "src/assistant_ws.c"
    "src/assistant_audio.c"
    "src/assistant_ui.c"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_wifi nvs_flash lvgl
    PRIV_REQUIRES esp_event driver freertos
)
