idf_component_register(
    SRCS "main.c" "bms_ble.c" "web_server.c" "config.c" "wifi_manager.c" "charge_ctrl.c" "stats.c"
         "wg_config.c" "wg_tunnel.c" "wg_web.c" "b64.c"
    INCLUDE_DIRS "."
    EMBED_FILES "index.html"
    REQUIRES bt nvs_flash esp_wifi esp_http_server esp_netif esp_event esp_coex driver app_update lwip esp_timer
)
