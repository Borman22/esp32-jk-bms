/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 Borman22 */

#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

/*
 * wg_web.h — HTTP API настройки WireGuard.
 *
 *   GET  /api/wg  → статус туннеля и текущие настройки (секретные ключи не возвращаются)
 *   POST /api/wg  → сохранить настройки; {"clear":true} — удалить конфигурацию туннеля
 *
 * Авторизацию выполняет функция, переданная при регистрации: ей отдаётся запрос,
 * при отказе она сама отправляет 401 и возвращает false.
 */

typedef bool (*wg_web_auth_fn)(httpd_req_t *req);

/** Зарегистрировать маршруты в уже запущенном HTTP-сервере (2 обработчика). */
void wg_web_register(httpd_handle_t server, wg_web_auth_fn auth);
