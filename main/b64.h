/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 Borman22 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * b64.h — минимальный декодер base64 для двух задач:
 *   - разбор заголовка HTTP Basic Auth (web_server.c);
 *   - проверка формата ключей WireGuard при вводе в веб-интерфейсе.
 */

/**
 * Декодировать base64-строку в out.
 * Возвращает длину результата в байтах или -1, если строка содержит
 * недопустимые символы или результат не помещается в out_size.
 * Если в out остаётся место, добавляется завершающий '\0'.
 */
int b64_decode(const char *in, char *out, size_t out_size);

/**
 * Проверить, что строка — ключ WireGuard: 44 символа base64, которые
 * декодируются ровно в 32 байта (так выглядит вывод `wg genkey` / `wg pubkey`).
 */
bool b64_is_wg_key(const char *s);
