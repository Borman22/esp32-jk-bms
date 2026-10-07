#include "wifi_manager.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1
#define MAX_RETRY            3       /* попыток переподключения перед AP */

/* Биты нужны только start_sta(): обработчик событий сообщает ему «получили IP» или «исчерпали попытки» */
static EventGroupHandle_t s_wifi_events;
static bool               s_ap_mode              = false;
static bool               s_connected            = false;
static bool               s_ever_connected       = false; /* true после первого успешного IP */
static bool               s_sta_bg_reconnect     = false; /* true: STA фоново переподключается в APSTA */
static int                s_retry                = 0;

/*
 * s_scan_only = true → STA_START не вызывает esp_wifi_connect().
 * Используется во время предварительного WiFi-скана.
 */
static volatile bool s_scan_only = false;

/* Результаты предварительного скана (до поднятия AP) */
static wifi_ap_t s_scan_result[WIFI_SCAN_MAX];
static int       s_scan_count  = 0;

/* ── Обработчик событий WiFi ─────────────────────────────────────────────── */

/*
 * Единая точка реакции на события WiFi/IP. Решает, переподключаться ли:
 *   - до первого успешного подключения — не больше MAX_RETRY попыток, потом FAIL
 *     (start_sta вернёт false и мы уйдём в AP);
 *   - после первого IP или в APSTA-режиме — бесконечно: пропал роутер или WiFi
 *     моргнул — устройство должно само вернуться в сеть.
 */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (!s_scan_only) esp_wifi_connect();

        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            if (s_scan_only) return;
            if (s_ever_connected || s_sta_bg_reconnect) {
                /* Переподключаемся бесконечно: либо уже был успешный коннект,
                 * либо STA работает фоново в APSTA-режиме. */
                esp_wifi_connect();
            } else if (s_retry < MAX_RETRY) {
                s_retry++;
                ESP_LOGI(TAG, "Retry %d/%d...", s_retry, MAX_RETRY);
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            }

        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            ESP_LOGI(TAG, "Client connected to AP");

        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            ESP_LOGI(TAG, "Client disconnected from AP");
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry          = 0;
        s_connected      = true;
        s_ever_connected = true;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* ── Скан WiFi ДО поднятия AP ────────────────────────────────────────────── */

/* Сортировка закэшированных результатов по убыванию RSSI (сильные сети сверху) */
static void sort_scan_cache(void)
{
    for (int i = 0; i < s_scan_count - 1; i++)
        for (int j = i + 1; j < s_scan_count; j++)
            if (s_scan_result[j].rssi > s_scan_result[i].rssi) {
                wifi_ap_t tmp = s_scan_result[i];
                s_scan_result[i] = s_scan_result[j];
                s_scan_result[j] = tmp;
            }
}

/*
 * Скан до поднятия AP: WiFi ещё не запущен, запускаем его в STA-режиме
 * только для скана, затем останавливаем.
 * Зачем так: радио одно, и скан при работающей AP прерывает её beacon'ы, что
 * рвёт соединение клиентов — а список сетей в настройках нужен именно тем, кто
 * подключился к AP. Поэтому сканируем заранее, пока клиентов нет, и кэшируем
 * результат в s_scan_result (отдаётся через wifi_get_scanned_aps()).
 */
static void prescan_wifi(void)
{
    ESP_LOGI(TAG, "Pre-scanning WiFi networks...");

    s_scan_only = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(1000));  /* дать радио стабилизироваться */

    s_scan_count = wifi_scan_aps(s_scan_result, WIFI_SCAN_MAX);
    ESP_LOGI(TAG, "Scan attempt 1: found %d networks", s_scan_count);

    /* Если ничего не нашли — повторить через 2 секунды */
    if (s_scan_count == 0) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        s_scan_count = wifi_scan_aps(s_scan_result, WIFI_SCAN_MAX);
        ESP_LOGI(TAG, "Scan attempt 2: found %d networks", s_scan_count);
    }

    sort_scan_cache();

    ESP_ERROR_CHECK(esp_wifi_stop());
    vTaskDelay(pdMS_TO_TICKS(200));
    s_scan_only = false;
}

/* ── Поднять точку доступа ───────────────────────────────────────────────── */

/*
 * AP называется BMS-Setup-XXXXXX (последние 3 байта MAC), чтобы несколько
 * устройств рядом не путались. Сеть открытая — это режим первичной настройки.
 */
static void start_ap(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X%02X",
             WIFI_AP_SSID_PREFIX, mac[3], mac[4], mac[5]);

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ssid);

    const app_config_t *saved = settings_get();
    if (saved->wifi_ssid[0] != '\0') {
        /*
         * Есть сохранённые credentials → APSTA: AP доступен локально,
         * STA в фоне бесконечно переподключается к роутеру.
         * Когда роутер вернётся — устройство снова станет доступно удалённо.
         */
        wifi_config_t sta_cfg = { .sta = { .listen_interval = 100 } };
        strlcpy((char *)sta_cfg.sta.ssid,     saved->wifi_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, saved->wifi_pass, sizeof(sta_cfg.sta.password));
        if (saved->wifi_pass[0] == '\0')
            sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

        s_sta_bg_reconnect = true;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,  &ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_ap_mode = true;
        ESP_LOGI(TAG, "AP started (APSTA): SSID='%s'  IP=192.168.4.1  STA→'%s'",
                 ssid, saved->wifi_ssid);
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_ap_mode = true;
        ESP_LOGI(TAG, "AP started: SSID='%s'  IP=192.168.4.1", ssid);
    }
}

/* ── Подключиться к WiFi как станция ─────────────────────────────────────── */

/*
 * Подключиться к сети и ждать результата до WIFI_CONNECT_TIMEOUT_MS.
 * true — получен IP; false — провал, WiFi остановлен (вызывающий поднимет AP).
 * Вызывается и для pending-, и для основных credentials, поэтому начало
 * сбрасывает состояние прошлой попытки.
 */
static bool start_sta(const char *ssid, const char *pass)
{
    /* Сбрасываем счётчик и биты от предыдущего вызова (например провал pending):
       иначе старый WIFI_FAIL_BIT заставил бы второй вызов сразу вернуть false */
    s_retry = 0;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    /* listen_interval=100 беконов (~1 с) — баланс между энергосбережением и латентностью.
     * Большее значение экономит больше энергии, но увеличивает задержку получения пакетов. */
    wifi_config_t sta_cfg = {
        .sta = { .listen_interval = 100 },
    };
    strlcpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));

    if (pass[0] == '\0')
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* MAX_MODEM: радио выключается между беконами. Экономит ~10 мА vs WIFI_PS_NONE.
     * Допустимо т.к. BLE имеет приоритет (esp_coex_preference_set), а SSE-трафик нечастый. */
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) return true;

    ESP_LOGW(TAG, "Failed to connect to '%s'", ssid);
    esp_wifi_stop();
    return false;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

/*
 * Выбор режима при старте (подробно — в README, «Режимы работы WiFi»):
 *   pending-сеть → сохранённая сеть → AP/APSTA. Блокирует вызывающего до
 * подключения или таймаута, поэтому BLE и веб стартуют уже после.
 */
void wifi_manager_init(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);

    const app_config_t *app_cfg = settings_get();

    /* ── Pending WiFi: новые credentials ожидают проверки ──────────────────
     * Пользователь сохранил новую сеть через веб-интерфейс.
     * Пробуем её первой; если удалось — делаем основной; если нет — удаляем
     * и продолжаем со старыми, чтобы устройство оставалось доступным.
     */
    char pend_ssid[CONFIG_WIFI_SSID_MAX_LEN];
    char pend_pass[CONFIG_WIFI_PASS_MAX_LEN];
    if (settings_load_pending_wifi(pend_ssid, sizeof(pend_ssid),
                                   pend_pass, sizeof(pend_pass))) {
        ESP_LOGI(TAG, "Trying pending WiFi: '%s'", pend_ssid);
        if (start_sta(pend_ssid, pend_pass)) {
            app_config_t new_cfg = *app_cfg;
            strlcpy(new_cfg.wifi_ssid, pend_ssid, sizeof(new_cfg.wifi_ssid));
            strlcpy(new_cfg.wifi_pass, pend_pass, sizeof(new_cfg.wifi_pass));
            settings_save(&new_cfg);
            settings_clear_pending_wifi();
            ESP_LOGI(TAG, "Pending WiFi confirmed, saved as main: '%s'", pend_ssid);
            s_scan_count = wifi_scan_aps(s_scan_result, WIFI_SCAN_MAX);
            sort_scan_cache();
            ESP_LOGI(TAG, "WiFi scan (STA): found %d networks", s_scan_count);
            return;
        }
        ESP_LOGW(TAG, "Pending WiFi '%s' failed — falling back to previous credentials",
                 pend_ssid);
        settings_clear_pending_wifi();
    }

    if (app_cfg->wifi_ssid[0] == '\0') {
        ESP_LOGI(TAG, "No WiFi config — scanning then starting AP");
        prescan_wifi();
        start_ap();
        return;
    }

    if (!start_sta(app_cfg->wifi_ssid, app_cfg->wifi_pass)) {
        ESP_LOGW(TAG, "WiFi connect failed — scanning then starting AP");
        prescan_wifi();
        start_ap();
        return;
    }

    /*
     * Скан в STA-режиме: ESP32 сканирует не отрывая соединения (null frames).
     * Без этого список сетей в настройках был бы пустым при работе через WiFi.
     */
    s_scan_count = wifi_scan_aps(s_scan_result, WIFI_SCAN_MAX);
    sort_scan_cache();
    ESP_LOGI(TAG, "WiFi scan (STA): found %d networks", s_scan_count);
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

bool wifi_manager_is_ap_mode(void)
{
    return s_ap_mode;
}

/* Кэш результатов скана (отсортирован по RSSI) — без нового сканирования, безопасно при работающей AP */
int wifi_get_scanned_aps(wifi_ap_t *out, int max_count)
{
    int n = s_scan_count < max_count ? s_scan_count : max_count;
    memcpy(out, s_scan_result, n * sizeof(wifi_ap_t));
    return n;
}

/* Выполнить скан сейчас (блокирующий). Дубликаты SSID схлопываются, скрытые сети пропускаются. */
int wifi_scan_aps(wifi_ap_t *out, int max_count)
{
    /* Пассивный скан: просто слушаем beacon'ы, не посылаем probe request'ы.
     * Более надёжен и работает даже если AP скрыта или плохо отвечает. */
    wifi_scan_config_t cfg = {
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time.passive = 200,   /* 200 мс на канал */
    };
    int n = 0;
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan start failed: %s", esp_err_to_name(err));
        goto done;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) goto done;

    uint16_t fetch = count < 40 ? count : 40;
    wifi_ap_record_t *recs = malloc(fetch * sizeof(wifi_ap_record_t));
    if (!recs) goto done;

    esp_wifi_scan_get_ap_records(&fetch, recs);

    for (int i = 0; i < fetch && n < max_count; i++) {
        if (recs[i].ssid[0] == '\0') continue;
        const char *ssid = (const char *)recs[i].ssid;
        /* Одна сеть может присутствовать несколько раз (разные точки доступа с одним SSID).
         * Дедуплицируем по SSID, оставляя лучший RSSI. */
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) {
                if (recs[i].rssi > out[j].rssi) out[j].rssi = recs[i].rssi;
                dup = true;
                break;
            }
        }
        if (!dup) {
            strlcpy(out[n].ssid, ssid, sizeof(out[n].ssid));
            out[n].rssi    = recs[i].rssi;
            out[n].secured = (recs[i].authmode != WIFI_AUTH_OPEN);
            n++;
        }
    }
    free(recs);

done:
    return n;
}
