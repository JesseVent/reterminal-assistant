idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_server esp_timer nvs_flash esp_netif esp_event
    EMBED_TXTFILES ../index.html
)
