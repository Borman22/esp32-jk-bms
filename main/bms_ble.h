#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t cell_voltage[8];    // mV
    uint16_t cell_resistance[8]; // mOhm
    uint16_t avg_voltage;        // mV
    uint16_t battery_voltage;    // mV
    uint8_t  soc;                // %
    uint16_t temp1;              // 0.1 °C
    uint16_t temp2;              // 0.1 °C
    uint16_t temp_mos;           // 0.1 °C
    uint32_t cap_remain;         // mAh
    uint32_t cap_full;           // mAh
    int32_t  power;              // mW
    int32_t  current;            // mA
    bool     valid;
} bms_data_t;

void bms_ble_init(void);
void bms_get_data(bms_data_t *out);

/* ── BLE device scan ─────────────────────────────────────────────────────── */

#define BLE_SEEN_MAX 20

typedef struct {
    char   addr[18];    /* "AA:BB:CC:DD:EE:FF" */
    char   name[32];    /* из advertising data, пусто если не известно */
    int8_t rssi;
    bool   is_jk_bms;  /* true если UUID 0xFFE0 в adv data или имя "JK-..." */
} ble_seen_t;

/* Вернуть список устройств замеченных при сканировании.
 * Возвращает количество записанных элементов (не более max_count). */
int bms_get_seen_devices(ble_seen_t *out, int max_count);
