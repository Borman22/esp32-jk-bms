#include "charge_ctrl.h"
#include "board_config.h"
#include "config.h"
#include "bms_ble.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "charge_ctrl";

#define NVS_NAMESPACE      "charge"
#define NVS_KEY_INPROG     "in_progress"
#define CHECK_INTERVAL_MS  5000   /* интервал проверки условий зарядки */

static volatile charge_state_t s_state             = CHARGE_STATE_IDLE;
static          TaskHandle_t   s_charge_task_handle = NULL;
static          uint8_t        s_active_gpio        = 0xFF; /* текущий выходной пин зарядника */

/* ── GPIO ──────────────────────────────────────────────────────────────────── */

/*
 * Установить состояние выхода зарядника с учётом полярности.
 * on=true  → включить зарядник
 * on=false → выключить зарядник
 */
static void set_charger(bool on)
{
    const app_config_t *cfg = settings_get();
    int level = (cfg->charger_active_high ? on : !on) ? 1 : 0;
    gpio_set_level(cfg->charger_gpio, level);
    gpio_set_level(BOARD_LED_GPIO, on ? 0 : 1);
}

/* ── NVS: флаг незавершённого цикла ───────────────────────────────────────── */

static void save_in_progress(bool in_progress)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_INPROG, in_progress ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static bool load_in_progress(void)
{
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_INPROG, &val);
        nvs_close(h);
    }
    return val != 0;
}

/* ── Вспомогательные ──────────────────────────────────────────────────────── */

/* Хотя бы одно условие старта задано (ненулевой порог) */
static bool any_start_configured(const app_config_t *cfg)
{
    return cfg->soc_start_pct > 0 || cfg->cell_min_start_mv > 0;
}

/* ── Условия START / STOP ──────────────────────────────────────────────────── */

/*
 * Вернуть true если нужно начать зарядку.
 * Проверяет SoC и минимальные напряжения ячеек.
 */
static bool should_start(const bms_data_t *d, const app_config_t *cfg,
                          char *reason, size_t reason_len)
{
    if (!d->valid) return false;
    if (cfg->soc_start_pct > 0 && d->soc <= cfg->soc_start_pct) {
        snprintf(reason, reason_len, "SoC=%d%% <= start=%d%%",
                 d->soc, cfg->soc_start_pct);
        return true;
    }
    if (cfg->cell_min_start_mv > 0) {
        for (int i = 0; i < 8; i++) {
            if (d->cell_voltage[i] > 0 &&
                d->cell_voltage[i] < cfg->cell_min_start_mv) {
                snprintf(reason, reason_len, "cell[%d]=%dmV < min_start=%dmV",
                         i + 1, d->cell_voltage[i], cfg->cell_min_start_mv);
                return true;
            }
        }
    }
    return false;
}

static bool should_stop(const bms_data_t *d, const app_config_t *cfg,
                         char *reason, size_t reason_len)
{
    if (!d->valid) return false;
    if (cfg->soc_stop_pct > 0 && d->soc >= cfg->soc_stop_pct) {
        snprintf(reason, reason_len, "SoC=%d%% >= stop=%d%%",
                 d->soc, cfg->soc_stop_pct);
        return true;
    }
    if (cfg->pack_stop_mv > 0 && d->battery_voltage >= cfg->pack_stop_mv) {
        snprintf(reason, reason_len, "vbat=%dmV >= pack_stop=%dmV",
                 d->battery_voltage, (int)cfg->pack_stop_mv);
        return true;
    }
    if (cfg->cell_max_stop_mv > 0) {
        for (int i = 0; i < 8; i++) {
            if (d->cell_voltage[i] > 0 &&
                d->cell_voltage[i] >= cfg->cell_max_stop_mv) {
                snprintf(reason, reason_len, "cell[%d]=%dmV >= max_stop=%dmV",
                         i + 1, d->cell_voltage[i], cfg->cell_max_stop_mv);
                return true;
            }
        }
    }
    return false;
}

/* ── Основная задача ───────────────────────────────────────────────────────── */

static void charge_task(void *arg)
{
    while (true) {
        bms_data_t d;
        bms_get_data(&d);
        const app_config_t *cfg = settings_get();

        char reason[64];

        switch (s_state) {

        case CHARGE_STATE_IDLE:
            /* Автостарт только если хотя бы одно условие задано */
            if (!any_start_configured(cfg)) break;

            if (should_start(&d, cfg, reason, sizeof(reason))) {
                ESP_LOGI(TAG, "Start charging: SoC=%d%%  vbat=%dmV  [%s]",
                         d.soc, d.battery_voltage, reason);
                s_state = CHARGE_STATE_CHARGING;
                save_in_progress(true);
                set_charger(true);
            }
            break;

        case CHARGE_STATE_CHARGING:
            /* Нет данных — не трогаем реле, ждём следующей итерации */
            if (!d.valid) break;

            if (should_stop(&d, cfg, reason, sizeof(reason))) {
                ESP_LOGI(TAG, "Stop charging: SoC=%d%%  vbat=%dmV  [%s]",
                         d.soc, d.battery_voltage, reason);
                s_state = CHARGE_STATE_IDLE;
                save_in_progress(false);
                set_charger(false);
            }
            break;
        }

        /* Ждём интервал, но выходим досрочно при уведомлении (смена настроек) */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_INTERVAL_MS));
    }
}

void charge_ctrl_notify_settings_changed(void)
{
    if (s_charge_task_handle)
        xTaskNotifyGive(s_charge_task_handle);
}

/* ── Публичный API ─────────────────────────────────────────────────────────── */

void charge_ctrl_apply_gpio_settings(void)
{
    const app_config_t *cfg = settings_get();

    /* Освободить старый пин: сначала 0 (очищаем регистр данных), затем INPUT */
    if (s_active_gpio != 0xFF && s_active_gpio != cfg->charger_gpio) {
        gpio_set_level(s_active_gpio, 0);
        gpio_config_t dis = {
            .pin_bit_mask = (1ULL << s_active_gpio),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&dis);
    }

    /* Настроить новый пин зарядника как выход */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << cfg->charger_gpio),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    s_active_gpio = cfg->charger_gpio;

    /* Применить текущее состояние зарядника к новому пину */
    set_charger(s_state == CHARGE_STATE_CHARGING);
}

void charge_ctrl_init(void)
{
    /* Светодиод — выход с постоянным назначением */
    gpio_config_t led_io = {
        .pin_bit_mask = (1ULL << BOARD_LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_io);
    gpio_set_level(BOARD_LED_GPIO, 1);

    /* Настроить GPIO зарядника и применить безопасное начальное состояние */
    charge_ctrl_apply_gpio_settings();

    /* Если при последней работе шла зарядка — продолжаем */
    if (load_in_progress()) {
        ESP_LOGI(TAG, "Resuming interrupted charge cycle");
        s_state = CHARGE_STATE_CHARGING;
        set_charger(true);
    } else {
        s_state = CHARGE_STATE_IDLE;
    }

    xTaskCreate(charge_task, "charge_ctrl", 3072, NULL, 5, &s_charge_task_handle);

    const app_config_t *cfg = settings_get();
    ESP_LOGI(TAG, "Charge controller ready: GPIO%d active_%s, state=%s",
             cfg->charger_gpio,
             cfg->charger_active_high ? "HIGH" : "LOW",
             s_state == CHARGE_STATE_CHARGING ? "CHARGING" : "IDLE");
}

charge_state_t charge_ctrl_get_state(void)
{
    return s_state;
}

void charge_ctrl_set_override(bool enable)
{
    if (enable) {
        ESP_LOGI(TAG, "Manual start: charging enabled");
        s_state = CHARGE_STATE_CHARGING;
        save_in_progress(true);
        set_charger(true);
    } else {
        ESP_LOGI(TAG, "Manual stop: charging disabled");
        s_state = CHARGE_STATE_IDLE;
        save_in_progress(false);
        set_charger(false);
    }
    charge_ctrl_notify_settings_changed();
}
