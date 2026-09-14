#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * wifi_manager.h — управление WiFi подключением.
 *
 * Логика запуска:
 *
 *   1. Если в конфиге нет сохранённого SSID (первое включение)
 *      → сразу поднять AP для настройки.
 *
 *   2. Если SSID есть → попытаться подключиться к WiFi.
 *      - Успех за WIFI_CONNECT_TIMEOUT_MS → нормальная работа.
 *      - Таймаут или неверный пароль → поднять AP для исправления настроек.
 *
 *   3. Если при старте удерживается кнопка BOOT (GPIO9) дольше
 *      BOOT_HOLD_MS → сбросить WiFi настройки и поднять AP.
 *
 * AP параметры:
 *   SSID: "BMS-Setup-XXXXXX"  (последние 3 байта MAC)
 *   Пароль: нет (открытая сеть для удобства первичной настройки)
 *   IP в AP режиме: 192.168.4.1
 */

#define WIFI_CONNECT_TIMEOUT_MS  15000   /* таймаут подключения к STA, мс */
#define BOOT_HOLD_MS             3000    /* удержание кнопки для сброса, мс */
#define WIFI_AP_SSID_PREFIX      "BMS-Setup"

/**
 * Инициализировать WiFi менеджер.
 * Читает конфиг, определяет режим (STA или AP) и подключается.
 * Блокирует выполнение до получения IP (STA) или поднятия AP.
 * Должна вызываться после settings_init().
 */
void wifi_manager_init(void);

/**
 * Вернуть true если устройство подключено к WiFi как станция (STA)
 * и имеет IP-адрес.
 */
bool wifi_manager_is_connected(void);

/**
 * Вернуть true если сейчас активен режим точки доступа (AP).
 * В AP-режиме веб-сервер доступен по адресу 192.168.4.1
 */
bool wifi_manager_is_ap_mode(void);

/* ── WiFi scan ───────────────────────────────────────────────────────────── */

#define WIFI_SCAN_MAX 20

typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secured;
} wifi_ap_t;

/**
 * Вернуть результаты WiFi-скана, выполненного при запуске устройства
 * (до поднятия AP, в STA-режиме — без прерывания клиентских соединений).
 * Всегда готовы после wifi_manager_init().
 */
int wifi_get_scanned_aps(wifi_ap_t *out, int max_count);

/**
 * Выполнить WiFi-скан прямо сейчас (~2с блокировки).
 * Работает только в STA или APSTA режиме.
 */
int wifi_scan_aps(wifi_ap_t *out, int max_count);
