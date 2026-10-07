#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * stats.h — журнал сессий разряда/заряда аккумулятора.
 *
 * Сессия определяется по току BMS (знак: + заряд, − разряд) с гистерезисом.
 * Одна сессия = одна запись; записи хранятся в NVS кольцом на STATS_MAX штук,
 * при переполнении затирается самая старая.
 *
 * Часов реального времени нет. Каждая запись содержит boot (номер загрузки)
 * и up_start (секунды от старта МК). Абсолютное время ts_start появляется,
 * когда часы синхронизированы (SNTP или POST /api/time): в этот момент
 * проставляется время всем записям текущей загрузки, включая прошлые.
 * ts_start == 0 → время неизвестно, показывать «загрузка N, +uptime».
 */

#define STATS_MAX 64

#define STATS_TYPE_DISCHARGE 1
#define STATS_TYPE_CHARGE    2

#define STATS_STATE_ACTIVE      0  /* сессия идёт */
#define STATS_STATE_DONE        1  /* завершена штатно */
#define STATS_STATE_INTERRUPTED 2  /* оборвана пропаданием питания */

typedef struct {
    uint8_t  type;
    uint8_t  state;
    uint16_t reserved;
    uint32_t boot;
    uint32_t ts_start;   /* unix-время начала, 0 — неизвестно */
    uint32_t up_start;   /* секунды от старта МК в момент начала */
    uint32_t dur_s;
    uint32_t mah_start;  /* cap_remain в начале */
    uint32_t mah_end;    /* cap_remain в конце (для активной — последний замер) */
} stats_rec_t;

/* Загрузить журнал из NVS и запустить задачу. После settings_init() и bms_ble_init(). */
void stats_init(void);

/* Копия журнала, новые первыми. out — массив на STATS_MAX. Возвращает число записей. */
int stats_snapshot(stats_rec_t *out, uint32_t *boot_id);

/* Часы установлены (SNTP или браузер). */
bool stats_clock_synced(void);
