/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 Borman22 */

#include "wg_web.h"
#include "wg_tunnel.h"
#include "wg_config.h"
#include "b64.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wg_web";

static wg_web_auth_fn s_auth;

#define MAX_BODY_LEN 1024

/* ── Вспомогательные функции ─────────────────────────────────────────────── */

/*
 * Прочитать тело запроса целиком и разобрать как JSON. httpd_req_recv() может
 * вернуть тело по частям, поэтому читаем в цикле. При ошибке сам отправляет 400
 * и возвращает NULL.
 */
static cJSON *read_json_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body size");
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive error");
            return NULL;
        }
        got += n;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return root;
}

/* Копирует строку из JSON-поля, если оно есть и является строкой. true — поле присутствовало. */
static bool json_get_str(const cJSON *obj, const char *key, char *dst, size_t dst_size)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsString(item)) return false;
    strlcpy(dst, item->valuestring, dst_size);
    return true;
}

/* Число из JSON-поля. true — поле есть и является числом. */
static bool json_get_num(const cJSON *obj, const char *key, int *out)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsNumber(item)) return false;
    *out = item->valueint;
    return true;
}

static bool valid_ipv4(const char *s)
{
    struct in_addr a;
    return inet_pton(AF_INET, s, &a) == 1;
}

/* Адрес сервера: IPv4 или DNS-имя из букв, цифр, точек и дефисов */
static bool valid_endpoint(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len >= CONFIG_WG_ENDPOINT_MAX_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static bool wg_equal(const wg_config_t *a, const wg_config_t *b)
{
    return strcmp(a->private_key,     b->private_key)     == 0 &&
           strcmp(a->peer_public_key, b->peer_public_key) == 0 &&
           strcmp(a->preshared_key,   b->preshared_key)   == 0 &&
           strcmp(a->endpoint,        b->endpoint)        == 0 &&
           strcmp(a->address,         b->address)         == 0 &&
           strcmp(a->netmask,         b->netmask)         == 0 &&
           a->port == b->port && a->keepalive == b->keepalive;
}

/* Что пользователь видел и менял последним: кандидат, если он есть, иначе основная. true — это кандидат. */
static bool effective_wg(wg_config_t *out)
{
    if (wg_config_load_pending(out)) return true;
    wg_config_load(out);
    return false;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *buf = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, -1);
    free(buf);
    return ESP_OK;
}

/* ── GET /api/wg ─────────────────────────────────────────────────────────── */

static esp_err_t wg_get_handler(httpd_req_t *req)
{
    if (!s_auth(req)) return ESP_OK;

    wg_status_t st;
    wg_tunnel_get_status(&st);
    const wg_config_t *active = wg_tunnel_active_config();

    wg_config_t wg;
    bool pending = effective_wg(&wg);

    cJSON *root = cJSON_CreateObject();

    /* Состояние работающего туннеля */
    cJSON *s = cJSON_AddObjectToObject(root, "status");
    cJSON_AddBoolToObject  (s, "configured",      wg_config_is_configured(active));
    cJSON_AddStringToObject(s, "state",           wg_state_name(st.state));
    cJSON_AddBoolToObject  (s, "trial",           st.trial);
    cJSON_AddBoolToObject  (s, "clock_ok",        st.clock_ok);
    cJSON_AddNumberToObject(s, "handshake_age_s", st.handshake_age_s);
    cJSON_AddStringToObject(s, "address",         active->address);
    cJSON_AddStringToObject(s, "endpoint",        active->endpoint);
    cJSON_AddNumberToObject(s, "port",            active->port);

    /* Секреты в браузер не отправляем — только признак «задан» */
    cJSON *w = cJSON_AddObjectToObject(root, "wg");
    cJSON_AddBoolToObject  (w, "private_key_set",   wg.private_key[0] != '\0');
    cJSON_AddStringToObject(w, "peer_public_key",   wg.peer_public_key);
    cJSON_AddBoolToObject  (w, "preshared_key_set", wg.preshared_key[0] != '\0');
    cJSON_AddStringToObject(w, "endpoint",          wg.endpoint);
    cJSON_AddNumberToObject(w, "port",              wg.port);
    cJSON_AddStringToObject(w, "address",           wg.address);
    cJSON_AddStringToObject(w, "netmask",           wg.netmask);
    cJSON_AddNumberToObject(w, "keepalive",         wg.keepalive);
    cJSON_AddBoolToObject  (root, "wg_pending",     pending);

    return send_json(req, root);
}

/* ── POST /api/wg ────────────────────────────────────────────────────────── */

/*
 * Применить поля запроса к копии настроек туннеля. false — значение не прошло
 * проверку, в *err — причина для пользователя.
 * Пустое или отсутствующее поле означает «не менять»: секреты в браузер не
 * возвращаются, поэтому форма не может прислать их обратно.
 */
static bool apply_wg_json(const cJSON *j, wg_config_t *wg, const char **err)
{
    char tmp[CONFIG_WG_ENDPOINT_MAX_LEN];
    int  num;

    if (json_get_str(j, "private_key", tmp, sizeof(tmp)) && tmp[0]) {
        if (!b64_is_wg_key(tmp)) { *err = "Приватный ключ: ожидается 44 символа base64"; return false; }
        strlcpy(wg->private_key, tmp, sizeof(wg->private_key));
    }
    if (json_get_str(j, "peer_public_key", tmp, sizeof(tmp)) && tmp[0]) {
        if (!b64_is_wg_key(tmp)) { *err = "Публичный ключ сервера: ожидается 44 символа base64"; return false; }
        strlcpy(wg->peer_public_key, tmp, sizeof(wg->peer_public_key));
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "preshared_clear"))) {
        wg->preshared_key[0] = '\0';
    } else if (json_get_str(j, "preshared_key", tmp, sizeof(tmp)) && tmp[0]) {
        if (!b64_is_wg_key(tmp)) { *err = "Preshared key: ожидается 44 символа base64"; return false; }
        strlcpy(wg->preshared_key, tmp, sizeof(wg->preshared_key));
    }
    if (json_get_str(j, "endpoint", tmp, sizeof(tmp)) && tmp[0]) {
        if (!valid_endpoint(tmp)) { *err = "Адрес сервера: IP или имя из букв, цифр, точек и дефисов"; return false; }
        strlcpy(wg->endpoint, tmp, sizeof(wg->endpoint));
    }
    if (json_get_num(j, "port", &num) && num != 0) {
        if (num < 1 || num > 65535) { *err = "Порт сервера: 1-65535"; return false; }
        wg->port = (uint16_t)num;
    }
    if (json_get_str(j, "address", tmp, sizeof(tmp)) && tmp[0]) {
        if (!valid_ipv4(tmp)) { *err = "Адрес в туннеле: ожидается IPv4"; return false; }
        strlcpy(wg->address, tmp, sizeof(wg->address));
    }
    if (json_get_str(j, "netmask", tmp, sizeof(tmp)) && tmp[0]) {
        if (!valid_ipv4(tmp)) { *err = "Маска туннеля: ожидается IPv4-маска"; return false; }
        strlcpy(wg->netmask, tmp, sizeof(wg->netmask));
    }
    if (json_get_num(j, "keepalive", &num)) {
        if (num < 0 || num > 65535) { *err = "Keepalive: 0-65535 секунд"; return false; }
        wg->keepalive = (uint16_t)num;
    }
    return true;
}

/*
 * Если туннель уже был настроен, новая конфигурация идёт кандидатом и откатится
 * сама, когда рукопожатие не состоится (см. wg_tunnel.h). Первая настройка
 * сохраняется сразу как основная: откатываться не к чему.
 * Туннель читает настройки при старте, поэтому после изменений нужна перезагрузка.
 */
static esp_err_t wg_post_handler(httpd_req_t *req)
{
    if (!s_auth(req)) return ESP_OK;
    cJSON *root = read_json_body(req);
    if (!root) return ESP_FAIL;

    const char *err = NULL;
    bool changed = false, pending = false;

    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "clear"))) {
        /* Полное отключение VPN: стираем основную конфигурацию и кандидата */
        cJSON_Delete(root);
        wg_config_reset();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":true,\"restart_required\":true,\"wg_pending\":false}", -1);
        return ESP_OK;
    }

    wg_config_t wg_orig, wg_new, wg_main;
    effective_wg(&wg_orig);
    wg_new = wg_orig;
    if (!apply_wg_json(root, &wg_new, &err)) goto fail;
    changed = !wg_equal(&wg_orig, &wg_new);
    if (changed && !wg_config_is_configured(&wg_new)) {
        err = "Заполните ключи, адрес сервера и адрес в туннеле";
        goto fail;
    }
    cJSON_Delete(root);

    if (changed) {
        wg_config_load(&wg_main);
        if (!wg_config_is_configured(&wg_main)) {
            wg_config_save(&wg_new);
            wg_config_clear_pending();
        } else if (wg_equal(&wg_new, &wg_main)) {
            /* Вернули прежние значения: кандидат больше не нужен */
            wg_config_clear_pending();
        } else {
            wg_config_save_pending(&wg_new);
            pending = true;
        }
    }
    ESP_LOGI(TAG, "WireGuard settings %s", changed ? (pending ? "saved as candidate" : "saved") : "unchanged");

    char resp[72];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"restart_required\":%s,\"wg_pending\":%s}",
             changed ? "true" : "false", pending ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;

fail:
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
    return ESP_OK;
}

/* ── Регистрация ─────────────────────────────────────────────────────────── */

void wg_web_register(httpd_handle_t server, wg_web_auth_fn auth)
{
    s_auth = auth;
    const httpd_uri_t routes[] = {
        { .uri = "/api/wg", .method = HTTP_GET,  .handler = wg_get_handler  },
        { .uri = "/api/wg", .method = HTTP_POST, .handler = wg_post_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(server, &routes[i]);
}
