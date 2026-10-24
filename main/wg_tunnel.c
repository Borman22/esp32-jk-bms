/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 Borman22 */

#include "wg_tunnel.h"
#include "wifi_manager.h"
#include "esp_wireguard.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <time.h>

static const char *TAG = "wg_tunnel";

/*
 * Активная конфигурация. Библиотека WireGuard хранит указатели на строки, а не
 * копии, поэтому s_wg и s_lib_cfg живут всё время работы программы.
 */
static wg_config_t        s_wg;
static wireguard_config_t s_lib_cfg;
static wireguard_ctx_t    s_ctx;

static bool s_configured = false;   /* заданы все обязательные поля */
static bool s_trial      = false;   /* работаем с кандидатом, ещё не подтверждённым */
static bool s_inited     = false;   /* esp_wireguard_init() уже вызван (повторно нельзя) */
static bool s_connected  = false;   /* туннель создан (не значит, что рукопожатие прошло) */

static volatile wg_state_t s_state = WG_STATE_DISABLED;

/* ── Вспомогательные функции ─────────────────────────────────────────────── */

bool wg_clock_is_valid(void)
{
    return time(NULL) >= WG_MIN_VALID_EPOCH;
}

const char *wg_state_name(wg_state_t state)
{
    switch (state) {
    case WG_STATE_DISABLED:   return "disabled";
    case WG_STATE_WAIT_WIFI:  return "wait_wifi";
    case WG_STATE_WAIT_TIME:  return "wait_time";
    case WG_STATE_CONNECTING: return "connecting";
    case WG_STATE_UP:         return "up";
    }
    return "unknown";
}

const wg_config_t *wg_tunnel_active_config(void)
{
    return &s_wg;
}

/* Заполнить структуру библиотеки указателями на s_wg */
static void fill_lib_config(void)
{
    s_lib_cfg = (wireguard_config_t)ESP_WIREGUARD_CONFIG_DEFAULT();
    s_lib_cfg.private_key          = s_wg.private_key;
    s_lib_cfg.public_key           = s_wg.peer_public_key;
    s_lib_cfg.preshared_key        = s_wg.preshared_key[0] ? s_wg.preshared_key : NULL;
    s_lib_cfg.address              = s_wg.address;
    s_lib_cfg.netmask              = s_wg.netmask;
    s_lib_cfg.endpoint             = s_wg.endpoint;
    s_lib_cfg.port                 = s_wg.port;
    s_lib_cfg.persistent_keepalive = s_wg.keepalive;

    s_ctx = (wireguard_ctx_t)ESP_WIREGUARD_CONTEXT_DEFAULT();
}

/*
 * Разрешить в туннеле всю подсеть (адрес & маска). По умолчанию библиотека
 * пускает через туннель только адрес самого устройства; без этого ответы на
 * запросы с вашего компьютера (например 10.8.0.3) уходят в никуда.
 */
static void allow_tunnel_subnet(void)
{
    struct in_addr addr, mask;
    if (inet_pton(AF_INET, s_wg.address, &addr) != 1 ||
        inet_pton(AF_INET, s_wg.netmask, &mask) != 1) {
        ESP_LOGW(TAG, "Bad tunnel address/netmask, subnet not allowed");
        return;
    }
    struct in_addr net = { .s_addr = addr.s_addr & mask.s_addr };

    char net_str[CONFIG_IPV4_STR_LEN];
    inet_ntop(AF_INET, &net, net_str, sizeof(net_str));

    if (esp_wireguard_add_allowed_ip(&s_ctx, net_str, s_wg.netmask) != ESP_OK)
        ESP_LOGW(TAG, "Failed to allow subnet %s/%s", net_str, s_wg.netmask);
    else
        ESP_LOGI(TAG, "Allowed subnet %s/%s", net_str, s_wg.netmask);
}

static void restart_after_log(const char *reason)
{
    ESP_LOGW(TAG, "Restarting: %s", reason);
    vTaskDelay(pdMS_TO_TICKS(500));   /* дать строке лога уйти в UART */
    esp_restart();
}

/* ── Задача-супервайзер ──────────────────────────────────────────────────── */

static void wg_task(void *arg)
{
    int64_t connect_us      = 0;                     /* когда туннель создан последний раз */
    int64_t last_up_us      = 0;                     /* когда в последний раз видели рукопожатие */
    int64_t trial_start_us  = 0;                     /* когда началась проверка кандидата */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();

        if (!s_configured) {
            s_state = WG_STATE_DISABLED;
            continue;
        }

        /* Пока нет WiFi или времени, проверять туннель не из чего: счётчики не идут */
        if (!wifi_manager_is_connected()) {
            s_state = WG_STATE_WAIT_WIFI;
            continue;
        }
        if (!wg_clock_is_valid()) {
            s_state = WG_STATE_WAIT_TIME;
            continue;
        }

        /* Таймер проверки кандидата стартует, когда у туннеля впервые появилась возможность подняться */
        if (s_trial && trial_start_us == 0) trial_start_us = now;
        if (s_trial && now - trial_start_us > (int64_t)WG_TRIAL_TIMEOUT_S * 1000000LL) {
            ESP_LOGE(TAG, "Candidate config failed: no handshake in %d s - rolling back", WG_TRIAL_TIMEOUT_S);
            wg_config_clear_pending();
            restart_after_log("rollback to previous WireGuard config");
        }

        if (!s_inited) {
            if (esp_wireguard_init(&s_lib_cfg, &s_ctx) != ESP_OK) {
                ESP_LOGE(TAG, "esp_wireguard_init failed, retry in 10 s");
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            s_inited = true;
        }

        if (!s_connected) {
            esp_err_t err = esp_wireguard_connect(&s_ctx);
            if (err == ESP_ERR_RETRY) {            /* DNS-имя сервера ещё резолвится */
                s_state = WG_STATE_CONNECTING;
                continue;
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_wireguard_connect: %s", esp_err_to_name(err));
                s_state = WG_STATE_CONNECTING;
                continue;                           /* повтор через секунду */
            }
            allow_tunnel_subnet();
            s_connected = true;
            connect_us  = now;
            s_state     = WG_STATE_CONNECTING;
            ESP_LOGI(TAG, "Tunnel created, waiting for handshake with %s:%u",
                     s_wg.endpoint, s_wg.port);
            continue;
        }

        if (esp_wireguard_peer_is_up(&s_ctx) == ESP_OK) {
            if (s_state != WG_STATE_UP) ESP_LOGI(TAG, "Tunnel is UP (ip %s)", s_wg.address);
            s_state    = WG_STATE_UP;
            last_up_us = now;
            if (s_trial) {
                wg_config_promote_pending();
                s_trial = false;
            }
            continue;
        }

        /* Рукопожатия нет: если ждём слишком долго — пересоздаём туннель */
        s_state = WG_STATE_CONNECTING;
        int64_t ref = connect_us > last_up_us ? connect_us : last_up_us;
        if (now - ref > (int64_t)WG_RECONNECT_AFTER_S * 1000000LL) {
            ESP_LOGW(TAG, "No handshake for %d s - recreating tunnel", WG_RECONNECT_AFTER_S);
            esp_wireguard_disconnect(&s_ctx);
            s_connected = false;
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void wg_tunnel_start(void)
{
    /* Кандидат важнее основной конфигурации: его надо проверить */
    wg_config_t pending;
    if (wg_config_load_pending(&pending)) {
        s_wg    = pending;
        s_trial = true;
        ESP_LOGI(TAG, "Trying candidate WireGuard config: %s:%u", s_wg.endpoint, s_wg.port);
    } else {
        wg_config_load(&s_wg);
    }

    s_configured = wg_config_is_configured(&s_wg);
    if (!s_configured) {
        ESP_LOGW(TAG, "WireGuard is not configured - open the setup page");
    }
    fill_lib_config();

    /* SNTP здесь не запускается: им занимается stats.c, время может прийти и из браузера */

    xTaskCreate(wg_task, "wg_task", 6144, NULL, 5, NULL);
}

void wg_tunnel_get_status(wg_status_t *out)
{
    out->state    = s_state;
    out->trial    = s_trial;
    out->clock_ok = wg_clock_is_valid();
    out->handshake_age_s = -1;

    if (s_state == WG_STATE_UP || s_state == WG_STATE_CONNECTING) {
        time_t hs;
        if (s_connected && esp_wireguard_latest_handshake(&s_ctx, &hs) == ESP_OK && out->clock_ok)
            out->handshake_age_s = (int32_t)(time(NULL) - hs);
    }
}
