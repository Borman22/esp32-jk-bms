/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 Borman22 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * wg_config.h — настройки WireGuard-туннеля в NVS.
 *
 * Хранятся отдельно от app_config_t (config.h): модуль WireGuard целиком
 * независим от остальной прошивки. Два пространства NVS:
 *   - "wg_cfg"  — основная (подтверждённая) конфигурация;
 *   - "wg_pend" — кандидат. Новая конфигурация, если туннель уже был настроен,
 *                 сначала попадает сюда и при следующей загрузке запускается
 *                 «на пробу». Нет рукопожатия за WG_TRIAL_TIMEOUT_S → кандидат
 *                 удаляется и устройство перезагружается со старой конфигурацией.
 */

#define CONFIG_WG_KEY_LEN            45   /* ключ WireGuard в base64: 44 символа + '\0' */
#define CONFIG_WG_ENDPOINT_MAX_LEN   64   /* IP-адрес или имя хоста VPN-сервера */
#define CONFIG_IPV4_STR_LEN          16   /* "255.255.255.255" + '\0' */

#define CONFIG_WG_PORT_DEFAULT        51820
#define CONFIG_WG_KEEPALIVE_DEFAULT   25            /* с; держит открытым NAT-соединение у провайдера */
#define CONFIG_WG_NETMASK_DEFAULT     "255.255.255.0"

typedef struct {
    char     private_key[CONFIG_WG_KEY_LEN];           /* приватный ключ ЭТОГО устройства */
    char     peer_public_key[CONFIG_WG_KEY_LEN];      /* публичный ключ VPN-сервера */
    char     preshared_key[CONFIG_WG_KEY_LEN];        /* необязательный; пусто = не используется */
    char     endpoint[CONFIG_WG_ENDPOINT_MAX_LEN];    /* адрес VPN-сервера (IP или DNS-имя) */
    uint16_t port;                                    /* UDP-порт VPN-сервера */
    char     address[CONFIG_IPV4_STR_LEN];            /* адрес устройства внутри туннеля, напр. 10.8.0.2 */
    char     netmask[CONFIG_IPV4_STR_LEN];            /* маска туннельной сети, напр. 255.255.255.0 */
    uint16_t keepalive;                               /* с; 0 = выключено */
} wg_config_t;

/** Заводские значения (порт, keepalive, маска; всё остальное пусто). */
void wg_config_set_defaults(wg_config_t *wg);

/** Загрузить основную конфигурацию. Нет сохранённой — заводские значения. */
void wg_config_load(wg_config_t *wg);

/** Сохранить основную конфигурацию. */
void wg_config_save(const wg_config_t *wg);

/** true, если заданы все обязательные поля (иначе туннель не запускается). */
bool wg_config_is_configured(const wg_config_t *wg);

/** Стереть основную конфигурацию и кандидата (сброс по BOOT, кнопка «Отключить»). */
void wg_config_reset(void);

/* ── Кандидат (двухэтапная фиксация) ──────────────────────────────────────── */

void wg_config_save_pending(const wg_config_t *wg);
bool wg_config_load_pending(wg_config_t *wg);
void wg_config_clear_pending(void);

/** Кандидат подтверждён: сделать его основным и удалить из пространства кандидатов. */
void wg_config_promote_pending(void);
