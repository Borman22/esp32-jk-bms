/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 Borman22 */

#include "b64.h"
#include <string.h>

/* Значение символа base64 (0–63) или -1, если символ не из алфавита */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int b64_decode(const char *in, char *out, size_t out_size)
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

bool b64_is_wg_key(const char *s)
{
    /* 32 байта в base64 = 44 символа, последний — '=' (дополнение) */
    if (strlen(s) != 44 || s[43] != '=') return false;
    char raw[40];
    return b64_decode(s, raw, sizeof(raw)) == 32;
}
