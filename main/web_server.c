#include "web_server.h"
#include "bms_ble.h"
#include "charge_ctrl.h"
#include "config.h"
#include "wifi_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "charge_ctrl.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web_server";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

/* ── Log ring buffer ─────────────────────────────────────────────────────── */

#define LOG_BUF_COUNT  80
#define LOG_BUF_LEN    160

typedef struct { uint32_t seq; char text[LOG_BUF_LEN]; } log_entry_t;

static log_entry_t  s_log_buf[LOG_BUF_COUNT];
static uint32_t     s_log_seq = 0;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

static void strip_ansi(char *s)
{
    char *dst = s;
    while (*s) {
        if (*s == '\033' && *(s + 1) == '[') {
            s += 2;
            while (*s && *s != 'm') s++;
            if (*s) s++;
        } else {
            *dst++ = *s++;
        }
    }
    *dst = '\0';
}

static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* va_list можно пройти только один раз: vprintf исчерпывает args,
     * поэтому копируем до его вызова для последующего vsnprintf. */
    va_list args2;
    va_copy(args2, args);
    int ret = vprintf(fmt, args);

    char tmp[LOG_BUF_LEN];
    vsnprintf(tmp, sizeof(tmp), fmt, args2);
    va_end(args2);

    strip_ansi(tmp);
    int len = (int)strlen(tmp);
    while (len > 0 && (tmp[len - 1] == '\n' || tmp[len - 1] == '\r'))
        tmp[--len] = '\0';
    if (len == 0) return ret;

    portENTER_CRITICAL(&s_log_mux);
    uint32_t idx = s_log_seq % LOG_BUF_COUNT;
    s_log_buf[idx].seq = s_log_seq;
    memcpy(s_log_buf[idx].text, tmp, (size_t)len + 1);
    s_log_seq++;
    portEXIT_CRITICAL(&s_log_mux);

    return ret;
}

void web_server_log_init(void)
{
    esp_log_set_vprintf(log_vprintf_hook);
}

/* ── SSE async state ─────────────────────────────────────────────────────── */

#define SSE_MAX_CLIENTS 4

static SemaphoreHandle_t s_sse_mutex                    = NULL;
static httpd_req_t      *s_sse_clients[SSE_MAX_CLIENTS] = {NULL};

/* ── Вспомогательная функция: BMS данные → JSON-строка ───────────────────── */

/*
 * Формирует JSON с данными BMS в буфере dst.
 * Используется и в /api/data и в SSE-потоке.
 * Возвращает длину записанной строки.
 */
static int bms_data_to_json(char *dst, size_t max_len)
{
    bms_data_t d;
    bms_get_data(&d);

    return snprintf(dst, max_len,
        "{"
        "\"valid\":%s,"
        "\"soc\":%d,"
        "\"battery_voltage\":%d,"
        "\"avg_voltage\":%d,"
        "\"current\":%ld,"
        "\"power\":%ld,"
        "\"temp1\":%d,"
        "\"temp2\":%d,"
        "\"temp_mos\":%d,"
        "\"cap_remain\":%lu,"
        "\"cap_full\":%lu,"
        "\"cell_voltages\":[%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"cell_resistances\":[%d,%d,%d,%d,%d,%d,%d,%d],"
        "\"charger_on\":%s"
        "}",
        d.valid ? "true" : "false",
        d.soc,
        d.battery_voltage,
        d.avg_voltage,
        (long)d.current,
        (long)d.power,
        d.temp1, d.temp2, d.temp_mos,
        (unsigned long)d.cap_remain,
        (unsigned long)d.cap_full,
        d.cell_voltage[0], d.cell_voltage[1], d.cell_voltage[2], d.cell_voltage[3],
        d.cell_voltage[4], d.cell_voltage[5], d.cell_voltage[6], d.cell_voltage[7],
        d.cell_resistance[0], d.cell_resistance[1], d.cell_resistance[2], d.cell_resistance[3],
        d.cell_resistance[4], d.cell_resistance[5], d.cell_resistance[6], d.cell_resistance[7],
        charge_ctrl_get_state() == CHARGE_STATE_CHARGING ? "true" : "false"
    );
}

/* ── HTTP Basic Auth ─────────────────────────────────────────────────────── */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Декодирует base64 строку. Возвращает длину результата или -1 при ошибке. */
static int base64_decode(const char *in, char *out, size_t out_size)
{
    size_t n = 0;
    while (in[0] && in[1]) {
        int a = b64_val(in[0]), b = b64_val(in[1]);
        if (a < 0 || b < 0) return -1;
        if (n >= out_size) return -1;
        out[n++] = (char)((a << 2) | (b >> 4));
        if (in[2] == '=' || in[2] == '\0') break;
        int c = b64_val(in[2]);
        if (c < 0) return -1;
        if (n >= out_size) return -1;
        out[n++] = (char)(((b & 0xf) << 4) | (c >> 2));
        if (in[3] == '=' || in[3] == '\0') break;
        int d = b64_val(in[3]);
        if (d < 0) return -1;
        if (n >= out_size) return -1;
        out[n++] = (char)(((c & 0x3) << 6) | d);
        in += 4;
    }
    if (n < out_size) out[n] = '\0';
    return (int)n;
}

/*
 * Проверяет Authorization: Basic ... заголовок.
 * Если пароль не задан (auth_pass пустой) — пропускает всех без проверки.
 * При отказе сам отправляет 401 и возвращает false.
 */
static bool check_auth(httpd_req_t *req)
{
    const app_config_t *cfg = settings_get();
    if (cfg->auth_pass[0] == '\0') return true;

    char hdr[200];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        goto deny;
    if (strncmp(hdr, "Basic ", 6) != 0) goto deny;

    char decoded[128];
    if (base64_decode(hdr + 6, decoded, sizeof(decoded)) < 0)
        goto deny;

    char *colon = strchr(decoded, ':');
    if (!colon) goto deny;
    *colon = '\0';
    const char *got_user = decoded;
    const char *got_pass = colon + 1;

    const char *want_user = cfg->auth_user[0] ? cfg->auth_user : "admin";
    if (strcmp(got_user, want_user) == 0 && strcmp(got_pass, cfg->auth_pass) == 0)
        return true;

deny:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"BMS\"");
    httpd_resp_send(req, "Unauthorized", -1);
    return false;
}

/* ── GET / ───────────────────────────────────────────────────────────────── */

static esp_err_t root_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html_start, (ssize_t)len);
    return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── GET /api/data ───────────────────────────────────────────────────────── */

static esp_err_t api_data_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    char json[680];
    int len = bms_data_to_json(json, sizeof(json));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

/* ── GET /api/events  (Server-Sent Events) ───────────────────────────────── */

/*
 * SSE использует async-handler API: sse_handler сразу возвращает управление,
 * не блокируя httpd-задачу. Данные отправляет отдельная sse_task.
 * Это позволяет httpd обрабатывать другие запросы (POST /api/settings и т.д.)
 * пока SSE соединение активно.
 */
static void sse_task(void *arg)
{
    char buf[700];
    char json[680];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Снимаем снимок списка клиентов под мьютексом */
        httpd_req_t *clients[SSE_MAX_CLIENTS];
        xSemaphoreTake(s_sse_mutex, portMAX_DELAY);
        memcpy(clients, s_sse_clients, sizeof(clients));
        xSemaphoreGive(s_sse_mutex);

        /* Проверяем, есть ли хоть один активный клиент */
        bool any = false;
        for (int i = 0; i < SSE_MAX_CLIENTS; i++) if (clients[i]) { any = true; break; }
        if (!any) continue;

        bms_data_to_json(json, sizeof(json));
        int len = snprintf(buf, sizeof(buf), "data: %s\n\n", json);

        for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
            if (!clients[i]) continue;
            if (httpd_resp_send_chunk(clients[i], buf, len) != ESP_OK) {
                xSemaphoreTake(s_sse_mutex, portMAX_DELAY);
                if (s_sse_clients[i] == clients[i]) {
                    httpd_req_async_handler_complete(s_sse_clients[i]);
                    s_sse_clients[i] = NULL;
                }
                xSemaphoreGive(s_sse_mutex);
            }
        }
    }
}

static esp_err_t sse_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    httpd_req_t *async_req;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(async_req, "text/event-stream");
    httpd_resp_set_hdr(async_req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(async_req, "Connection", "keep-alive");
    httpd_resp_set_hdr(async_req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(async_req, "X-Accel-Buffering", "no");

    /* retry указывает браузеру подождать 5 с перед переподключением при разрыве SSE */
    if (httpd_resp_send_chunk(async_req, "retry: 5000\n\n", -1) != ESP_OK) {
        httpd_req_async_handler_complete(async_req);
        return ESP_OK;
    }

    xSemaphoreTake(s_sse_mutex, portMAX_DELAY);
    /* Ищем свободный слот; если все заняты — вытесняем самый старый (слот 0) */
    int slot = -1;
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (!s_sse_clients[i]) { slot = i; break; }
    }
    if (slot < 0) {
        httpd_req_async_handler_complete(s_sse_clients[0]);
        for (int i = 0; i < SSE_MAX_CLIENTS - 1; i++)
            s_sse_clients[i] = s_sse_clients[i + 1];
        slot = SSE_MAX_CLIENTS - 1;
    }
    s_sse_clients[slot] = async_req;
    xSemaphoreGive(s_sse_mutex);

    return ESP_OK;
}

/* ── GET /api/settings ───────────────────────────────────────────────────── */

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    const app_config_t *cfg = settings_get();

    char mac_str[CONFIG_BMS_ADDR_STR_LEN];
    if (cfg->bms_addr_set) {
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 cfg->bms_addr[0], cfg->bms_addr[1], cfg->bms_addr[2],
                 cfg->bms_addr[3], cfg->bms_addr[4], cfg->bms_addr[5]);
    } else {
        mac_str[0] = '\0';
    }

    /* Пароли намеренно не отправляем обратно в браузер */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid",          cfg->wifi_ssid);
    cJSON_AddBoolToObject  (root, "auth_enabled",       cfg->auth_pass[0] != '\0');
    cJSON_AddStringToObject(root, "auth_user",          cfg->auth_user);
    cJSON_AddStringToObject(root, "bms_mac",            mac_str);
    cJSON_AddNumberToObject(root, "soc_start",          cfg->soc_start_pct);
    cJSON_AddNumberToObject(root, "soc_stop",           cfg->soc_stop_pct);
    cJSON_AddNumberToObject(root, "cell_min_start_mv",  cfg->cell_min_start_mv);
    cJSON_AddNumberToObject(root, "cell_max_stop_mv",   cfg->cell_max_stop_mv);
    cJSON_AddNumberToObject(root, "pack_stop_mv",       cfg->pack_stop_mv);
    cJSON_AddNumberToObject(root, "charger_gpio",       cfg->charger_gpio);
    cJSON_AddBoolToObject  (root, "charger_active_high", cfg->charger_active_high);

    char *buf = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, -1);
    free(buf);
    return ESP_OK;
}

/* ── POST /api/settings ──────────────────────────────────────────────────── */

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    /* Читаем тело запроса */
    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    /* Берём текущий конфиг как основу — изменяем только то, что пришло */
    app_config_t cfg = *settings_get();
    bool wifi_changed = false;
    cJSON *item;

    /* WiFi */
    if ((item = cJSON_GetObjectItem(root, "wifi_ssid")) && cJSON_IsString(item)) {
        if (strcmp(cfg.wifi_ssid, item->valuestring) != 0) wifi_changed = true;
        strlcpy(cfg.wifi_ssid, item->valuestring, sizeof(cfg.wifi_ssid));
    }
    if ((item = cJSON_GetObjectItem(root, "wifi_pass")) && cJSON_IsString(item)
        && item->valuestring[0] != '\0') {
        /* Пароль обновляем только если пришёл непустой */
        wifi_changed = true;
        strlcpy(cfg.wifi_pass, item->valuestring, sizeof(cfg.wifi_pass));
    }

    /* BMS MAC: строка "AA:BB:CC:DD:EE:FF" → байтовый массив */
    if ((item = cJSON_GetObjectItem(root, "bms_mac")) && cJSON_IsString(item)
        && strlen(item->valuestring) == 17) {
        unsigned int b[6];
        if (sscanf(item->valuestring, "%x:%x:%x:%x:%x:%x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            for (int i = 0; i < 6; i++) cfg.bms_addr[i] = (uint8_t)b[i];
            cfg.bms_addr_set = true;
        }
    }

    /* Пороги зарядки: 0 = условие отключено (пустое поле в UI) */
    if ((item = cJSON_GetObjectItem(root, "soc_start")))
        cfg.soc_start_pct    = cJSON_IsNumber(item) ? (uint8_t)item->valueint   : 0;
    if ((item = cJSON_GetObjectItem(root, "soc_stop")))
        cfg.soc_stop_pct     = cJSON_IsNumber(item) ? (uint8_t)item->valueint   : 0;
    if ((item = cJSON_GetObjectItem(root, "cell_min_start_mv")))
        cfg.cell_min_start_mv = cJSON_IsNumber(item) ? (uint16_t)item->valueint : 0;
    if ((item = cJSON_GetObjectItem(root, "cell_max_stop_mv")))
        cfg.cell_max_stop_mv  = cJSON_IsNumber(item) ? (uint16_t)item->valueint : 0;
    if ((item = cJSON_GetObjectItem(root, "pack_stop_mv")))
        cfg.pack_stop_mv      = cJSON_IsNumber(item) ? (uint32_t)item->valueint : 0;

    /* GPIO */
    if ((item = cJSON_GetObjectItem(root, "charger_gpio")) && cJSON_IsNumber(item))
        cfg.charger_gpio = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "charger_active_high")) && cJSON_IsBool(item))
        cfg.charger_active_high = cJSON_IsTrue(item);

    /* Auth */
    cJSON *auth_en = cJSON_GetObjectItem(root, "auth_enabled");
    if (auth_en && cJSON_IsBool(auth_en)) {
        if (cJSON_IsFalse(auth_en)) {
            /* Авторизация отключена — стираем credentials */
            cfg.auth_user[0] = '\0';
            cfg.auth_pass[0] = '\0';
        } else {
            if ((item = cJSON_GetObjectItem(root, "auth_user")) && cJSON_IsString(item))
                strlcpy(cfg.auth_user, item->valuestring, sizeof(cfg.auth_user));
            /* Пароль обновляем только если пришёл непустой */
            if ((item = cJSON_GetObjectItem(root, "auth_pass")) && cJSON_IsString(item)
                && item->valuestring[0] != '\0')
                strlcpy(cfg.auth_pass, item->valuestring, sizeof(cfg.auth_pass));
        }
    }

    cJSON_Delete(root);

    if (wifi_changed && cfg.wifi_ssid[0] != '\0') {
        /* Новые WiFi-данные сохраняем как «кандидат»: если пароль неверный,
         * устройство после перезагрузки вернётся к старым credentials и
         * останется доступным. Остальные настройки сохраняем сразу. */
        const app_config_t *cur = settings_get();
        app_config_t save_cfg = cfg;
        strlcpy(save_cfg.wifi_ssid, cur->wifi_ssid, sizeof(save_cfg.wifi_ssid));
        strlcpy(save_cfg.wifi_pass, cur->wifi_pass, sizeof(save_cfg.wifi_pass));
        settings_save(&save_cfg);
        settings_save_pending_wifi(cfg.wifi_ssid, cfg.wifi_pass);
    } else {
        settings_save(&cfg);
    }

    charge_ctrl_notify_settings_changed();

    /* Ответ: сообщить нужна ли перезагрузка для применения WiFi настроек */
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"restart_required\":%s}",
             wifi_changed ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);

    return ESP_OK;
}

/* ── GET /api/wifi/scan ──────────────────────────────────────────────────── */

/*
 * Скан выполняется однократно при старте устройства — в STA-режиме,
 * до поднятия AP, пока нет подключённых клиентов.
 * Здесь просто возвращаем закэшированные результаты.
 */
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    wifi_ap_t aps[WIFI_SCAN_MAX];
    int n = wifi_get_scanned_aps(aps, WIFI_SCAN_MAX);

    /* cJSON корректно экранирует кавычки, слеши и спецсимволы в SSID */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "ssid",    aps[i].ssid);
        cJSON_AddNumberToObject(obj, "rssi",    aps[i].rssi);
        cJSON_AddBoolToObject  (obj, "secured", aps[i].secured);
        cJSON_AddItemToArray(arr, obj);
    }
    char *buf = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, -1);
    free(buf);
    return ESP_OK;
}

/* ── GET /api/ble/scan ───────────────────────────────────────────────────── */

static esp_err_t ble_scan_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    ble_seen_t *devs = malloc(BLE_SEEN_MAX * sizeof(ble_seen_t));
    if (!devs) { httpd_resp_send_500(req); return ESP_FAIL; }

    int n = bms_get_seen_devices(devs, BLE_SEEN_MAX);

    /* Сортировка: сначала JK BMS, внутри групп по убыванию RSSI */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++) {
            bool swap = (!devs[i].is_jk_bms && devs[j].is_jk_bms) ||
                        (devs[i].is_jk_bms == devs[j].is_jk_bms &&
                         devs[j].rssi > devs[i].rssi);
            if (swap) {
                ble_seen_t tmp = devs[i]; devs[i] = devs[j]; devs[j] = tmp;
            }
        }

    /* cJSON корректно экранирует спецсимволы в именах BLE устройств */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "addr",  devs[i].addr);
        cJSON_AddStringToObject(obj, "name",  devs[i].name);
        cJSON_AddNumberToObject(obj, "rssi",  devs[i].rssi);
        cJSON_AddBoolToObject  (obj, "is_jk", devs[i].is_jk_bms);
        cJSON_AddItemToArray(arr, obj);
    }
    free(devs);

    char *buf = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, -1);
    free(buf);
    return ESP_OK;
}

/* ── POST /api/charger ───────────────────────────────────────────────────── */

static esp_err_t charger_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    char body[64];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *on = cJSON_GetObjectItem(root, "on");
    if (on && cJSON_IsBool(on))
        charge_ctrl_set_override(cJSON_IsTrue(on));
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", -1);
    return ESP_OK;
}

/* ── GET /api/logs ───────────────────────────────────────────────────────── */

static esp_err_t logs_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    char qbuf[64] = {0};
    uint32_t from = 0;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(qbuf, "from", val, sizeof(val)) == ESP_OK)
            from = (uint32_t)strtoul(val, NULL, 10);
    }

    portENTER_CRITICAL(&s_log_mux);
    uint32_t seq_end = s_log_seq;
    portEXIT_CRITICAL(&s_log_mux);

    uint32_t seq_start = (seq_end > LOG_BUF_COUNT) ? seq_end - LOG_BUF_COUNT : 0;
    if (from > seq_start) seq_start = from;

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();

    for (uint32_t s = seq_start; s < seq_end; s++) {
        uint32_t idx = s % LOG_BUF_COUNT;
        char text[LOG_BUF_LEN];
        uint32_t actual_seq;

        portENTER_CRITICAL(&s_log_mux);
        actual_seq = s_log_buf[idx].seq;
        memcpy(text, s_log_buf[idx].text, LOG_BUF_LEN);
        portEXIT_CRITICAL(&s_log_mux);

        /* Кольцевой буфер мог перезаписаться пока мы итерировали — пропускаем устаревший слот */
        if (actual_seq != s) continue;

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "seq", actual_seq);
        cJSON_AddStringToObject(entry, "msg", text);
        cJSON_AddItemToArray(arr, entry);
    }

    cJSON_AddItemToObject(root, "logs", arr);
    cJSON_AddNumberToObject(root, "next", seq_end);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json, -1);
    free(json);
    return ESP_OK;
}

/* ── POST /api/restart ───────────────────────────────────────────────────── */

/*
 * Отправляет ответ и через 500 мс перезагружает устройство.
 * Используется после изменения WiFi настроек.
 */
static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", -1);
    xTaskCreate(restart_task, "restart", 1024, NULL, 5, NULL);
    return ESP_OK;
}

/* ── Запуск сервера ───────────────────────────────────────────────────────── */

void web_server_start(void)
{
    s_sse_mutex = xSemaphoreCreateMutex();
    xTaskCreate(sse_task, "sse_task", 4096, NULL, 5, NULL);

    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = 16384;  /* SSE + scan handlers требуют увеличенного стека */
    config.max_open_sockets  = 7;      /* до 7 одновременных подключений (включая SSE) */
    config.lru_purge_enable  = true;   /* автозакрытие старых соединений при нехватке слотов */
    config.max_uri_handlers  = 12;  /* текущих маршрутов 11, одна единица запаса */

    httpd_handle_t server;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    /* Регистрируем все маршруты */
    const httpd_uri_t routes[] = {
        { .uri = "/",              .method = HTTP_GET,  .handler = root_handler          },
        { .uri = "/favicon.ico",   .method = HTTP_GET,  .handler = favicon_handler       },
        { .uri = "/api/data",      .method = HTTP_GET,  .handler = api_data_handler      },
        { .uri = "/api/events",    .method = HTTP_GET,  .handler = sse_handler           },
        { .uri = "/api/settings",  .method = HTTP_GET,  .handler = settings_get_handler  },
        { .uri = "/api/settings",  .method = HTTP_POST, .handler = settings_post_handler },
        { .uri = "/api/restart",   .method = HTTP_POST, .handler = restart_handler       },
        { .uri = "/api/wifi/scan", .method = HTTP_GET,  .handler = wifi_scan_handler     },
        { .uri = "/api/ble/scan",  .method = HTTP_GET,  .handler = ble_scan_handler      },
        { .uri = "/api/logs",      .method = HTTP_GET,  .handler = logs_handler          },
        { .uri = "/api/charger",   .method = HTTP_POST, .handler = charger_post_handler  },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(server, &routes[i]);

    ESP_LOGI(TAG, "HTTP server started on port 80");
}
