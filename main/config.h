#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * config.h — хранение и загрузка настроек устройства в NVS (Non-Volatile Storage).
 *
 * Все настройки переживают перезагрузку и отключение питания.
 * При первом включении (пустой NVS) используются значения по умолчанию.
 *
 * Структура разделена на логические группы:
 *   - WiFi: SSID и пароль домашней сети
 *   - BMS:  MAC-адрес целевого устройства
 *   - Charging: пороги включения/отключения зарядки
 *   - GPIO: номера выводов управления зарядником
 */

/* ── WiFi ─────────────────────────────────────────────────────────────────── */

#define CONFIG_WIFI_SSID_MAX_LEN     33   /* максимальная длина SSID + '\0' */
#define CONFIG_WIFI_PASS_MAX_LEN     65   /* максимальная длина пароля + '\0' */

/* ── BMS ──────────────────────────────────────────────────────────────────── */

#define CONFIG_BMS_ADDR_LEN          6    /* длина MAC-адреса в байтах */
#define CONFIG_BMS_ADDR_STR_LEN      18   /* "AA:BB:CC:DD:EE:FF" + '\0' */

/* ── Charging thresholds (пороги управления зарядкой) ────────────────────── */

/*
 * Логика зарядки:
 *
 *  START conditions (любое из):
 *    - SoC упал ниже soc_start_pct  (например 40%)
 *    - любая ячейка опустилась ниже cell_min_start_mv (например 3200 мВ)
 *
 *  STOP conditions (любое из):
 *    - SoC достиг soc_stop_pct  (например 95%)
 *    - любая ячейка поднялась выше cell_max_stop_mv (например 3600 мВ)
 *    - суммарное напряжение достигло pack_stop_mv (например 14000 мВ)
 *
 *  RESUME: если зарядка была начата и прервана (пропало питание),
 *  она возобновится при появлении питания, даже если SoC > soc_start_pct,
 *  пока не будет достигнуто условие STOP.
 */

/* значения по умолчанию */
#define CONFIG_SOC_START_DEFAULT          40    /* % — начинать зарядку */
#define CONFIG_SOC_STOP_DEFAULT           95    /* % — заканчивать зарядку */
#define CONFIG_CELL_MIN_START_MV_DEFAULT  3200  /* мВ — мин. напряжение ячейки для старта */
#define CONFIG_CELL_MAX_STOP_MV_DEFAULT   3600  /* мВ — макс. напряжение ячейки для стопа */
#define CONFIG_PACK_STOP_MV_DEFAULT       14000 /* мВ — макс. суммарное напряжение для стопа */

/* ── GPIO ─────────────────────────────────────────────────────────────────── */

/*
 * Пин управления зарядником.
 * active_high = true:  HIGH = зарядка включена, LOW = выключена
 * active_high = false: LOW  = зарядка включена, HIGH = выключена
 */
#define CONFIG_CHARGER_GPIO_DEFAULT       3     /* GPIO3 по умолчанию */
#define CONFIG_CHARGER_ACTIVE_HIGH_DEFAULT true

/* ══════════════════════════════════════════════════════════════════════════
 * Структура конфигурации
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    /* WiFi */
    char wifi_ssid[CONFIG_WIFI_SSID_MAX_LEN];
    char wifi_pass[CONFIG_WIFI_PASS_MAX_LEN];

    /* BMS */
    uint8_t bms_addr[CONFIG_BMS_ADDR_LEN];   /* MAC в big-endian (Bluedroid) */
    bool    bms_addr_set;                     /* false = адрес ещё не задан */

    /* Charging */
    uint8_t  soc_start_pct;        /* % — начинать зарядку */
    uint8_t  soc_stop_pct;         /* % — заканчивать зарядку */
    uint16_t cell_min_start_mv;    /* мВ — мин. напряжение ячейки для старта */
    uint16_t cell_max_stop_mv;     /* мВ — макс. напряжение ячейки для стопа */
    uint32_t pack_stop_mv;         /* мВ — макс. суммарное напряжение для стопа */

    /* GPIO */
    uint8_t charger_gpio;          /* номер GPIO управления зарядником */
    bool    charger_active_high;   /* полярность сигнала */
} app_config_t;

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Инициализировать модуль настроек.
 * Загружает сохранённые настройки из NVS.
 * Если NVS пуст — заполняет структуру значениями по умолчанию.
 * Должна вызываться один раз после nvs_flash_init().
 */
void settings_init(void);

/**
 * Получить указатель на текущие настройки (только для чтения).
 * Структура действительна до следующего вызова settings_save().
 */
const app_config_t *settings_get(void);

/**
 * Сохранить новые настройки в NVS и обновить текущие.
 * @param cfg  указатель на заполненную структуру настроек
 */
void settings_save(const app_config_t *cfg);

/**
 * Сбросить все настройки на заводские значения и сохранить в NVS.
 * После вызова устройство следует перезагрузить.
 */
void settings_reset(void);

/**
 * Сохранить новые WiFi-данные как «кандидат» в отдельное NVS-пространство.
 * Основные credentials (wifi_ssid / wifi_pass) при этом не меняются.
 * При следующей загрузке wifi_manager попробует кандидата первым;
 * если подключение удастся — кандидат станет основным; если нет — удалится.
 */
void settings_save_pending_wifi(const char *ssid, const char *pass);

/**
 * Загрузить pending WiFi из NVS.
 * Возвращает true если кандидат найден и непустой.
 */
bool settings_load_pending_wifi(char *ssid, size_t ssid_len,
                                char *pass, size_t pass_len);

/**
 * Удалить pending WiFi из NVS (вызывается после успеха или провала).
 */
void settings_clear_pending_wifi(void);
