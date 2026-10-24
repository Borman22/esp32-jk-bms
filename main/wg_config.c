/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 Borman22 */

#include "wg_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wg_config";

#define NVS_NS_WG       "wg_cfg"    /* основная конфигурация */
#define NVS_NS_WG_PEND  "wg_pend"   /* кандидат */

/*
 * Как и в config.c: ошибка чтения означает «ключа нет» (первый запуск или поле
 * из новой версии прошивки), в dst остаётся заводское значение.
 */
static void load_str(nvs_handle_t h, const char *key, char *dst, size_t max_len)
{
    size_t len = max_len;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK)
        dst[0] = '\0';
}

void wg_config_set_defaults(wg_config_t *wg)
{
    memset(wg, 0, sizeof(*wg));
    wg->port      = CONFIG_WG_PORT_DEFAULT;
    wg->keepalive = CONFIG_WG_KEEPALIVE_DEFAULT;
    strlcpy(wg->netmask, CONFIG_WG_NETMASK_DEFAULT, sizeof(wg->netmask));
}

/* Имена ключей NVS ограничены 15 символами. Одна пара функций обслуживает оба пространства. */
static void wg_load(nvs_handle_t h, wg_config_t *wg)
{
    load_str(h, "wg_priv",     wg->private_key,     sizeof(wg->private_key));
    load_str(h, "wg_pub",      wg->peer_public_key, sizeof(wg->peer_public_key));
    load_str(h, "wg_psk",      wg->preshared_key,   sizeof(wg->preshared_key));
    load_str(h, "wg_endpoint", wg->endpoint,        sizeof(wg->endpoint));
    nvs_get_u16(h, "wg_port",  &wg->port);
    load_str(h, "wg_addr",     wg->address,         sizeof(wg->address));

    /* Маска без сохранённого значения остаётся заводской, а не пустой */
    char mask[CONFIG_IPV4_STR_LEN];
    load_str(h, "wg_mask", mask, sizeof(mask));
    if (mask[0] != '\0') strlcpy(wg->netmask, mask, sizeof(wg->netmask));
    nvs_get_u16(h, "wg_keepalive", &wg->keepalive);
}

static void wg_store(nvs_handle_t h, const wg_config_t *wg)
{
    nvs_set_str(h, "wg_priv",     wg->private_key);
    nvs_set_str(h, "wg_pub",      wg->peer_public_key);
    nvs_set_str(h, "wg_psk",      wg->preshared_key);
    nvs_set_str(h, "wg_endpoint", wg->endpoint);
    nvs_set_u16(h, "wg_port",     wg->port);
    nvs_set_str(h, "wg_addr",     wg->address);
    nvs_set_str(h, "wg_mask",     wg->netmask);
    nvs_set_u16(h, "wg_keepalive", wg->keepalive);
}

static void erase_namespace(const char *ns)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

/* ── Основная конфигурация ───────────────────────────────────────────────── */

void wg_config_load(wg_config_t *wg)
{
    wg_config_set_defaults(wg);

    nvs_handle_t h;
    if (nvs_open(NVS_NS_WG, NVS_READONLY, &h) != ESP_OK) return;   /* не сохранялась */
    wg_load(h, wg);
    nvs_close(h);
}

void wg_config_save(const wg_config_t *wg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WG, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed, WireGuard config not saved");
        return;
    }
    wg_store(h, wg);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WireGuard config saved: endpoint='%s:%u'", wg->endpoint, wg->port);
}

bool wg_config_is_configured(const wg_config_t *wg)
{
    return wg->private_key[0] && wg->peer_public_key[0] &&
           wg->endpoint[0]    && wg->address[0] && wg->netmask[0] && wg->port;
}

void wg_config_reset(void)
{
    erase_namespace(NVS_NS_WG);
    erase_namespace(NVS_NS_WG_PEND);
    ESP_LOGI(TAG, "WireGuard config erased");
}

/* ── Кандидат ────────────────────────────────────────────────────────────── */

void wg_config_save_pending(const wg_config_t *wg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WG_PEND, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);           /* не оставляем ключи от предыдущего кандидата */
    wg_store(h, wg);
    nvs_set_u8(h, "valid", 1);  /* признак «кандидат есть»: ключи сами по себе могут быть пустыми */
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Pending WireGuard config saved: endpoint='%s:%u'", wg->endpoint, wg->port);
}

bool wg_config_load_pending(wg_config_t *wg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WG_PEND, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t valid = 0;
    if (nvs_get_u8(h, "valid", &valid) != ESP_OK || !valid) {
        nvs_close(h);
        return false;
    }
    wg_config_set_defaults(wg);
    wg_load(h, wg);
    nvs_close(h);
    return true;
}

void wg_config_clear_pending(void)
{
    erase_namespace(NVS_NS_WG_PEND);
    ESP_LOGI(TAG, "Pending WireGuard config cleared");
}

void wg_config_promote_pending(void)
{
    wg_config_t wg;
    if (!wg_config_load_pending(&wg)) return;
    wg_config_save(&wg);
    wg_config_clear_pending();
    ESP_LOGI(TAG, "Pending WireGuard config promoted to main");
}
