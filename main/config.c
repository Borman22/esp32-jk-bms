#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG       = "config";
static const char *NVS_NS    = "app_cfg";   /* namespace в NVS */

/* Текущая конфигурация в RAM — единственная копия, все модули читают её */
static app_config_t s_cfg;

/* ── Вспомогательные функции чтения/записи NVS ───────────────────────────── */

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t max_len)
{
    size_t len = max_len;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK)
        dst[0] = '\0';
}

static void load_u8(nvs_handle_t h, const char *key, uint8_t *dst)
{
    nvs_get_u8(h, key, dst); /* ошибка = ключ не найден, оставляем значение по умолчанию */
}

static void load_u16(nvs_handle_t h, const char *key, uint16_t *dst)
{
    nvs_get_u16(h, key, dst);
}

static void load_u32(nvs_handle_t h, const char *key, uint32_t *dst)
{
    nvs_get_u32(h, key, dst);
}

static void load_blob(nvs_handle_t h, const char *key, void *dst, size_t len)
{
    size_t actual = len;
    nvs_get_blob(h, key, dst, &actual);
}

/* ── Заполнить структуру заводскими значениями ───────────────────────────── */

static void set_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* WiFi — пусто, нужна первоначальная настройка */
    cfg->wifi_ssid[0] = '\0';
    cfg->wifi_pass[0] = '\0';

    /* Auth — по умолчанию без пароля */
    cfg->auth_user[0] = '\0';
    cfg->auth_pass[0] = '\0';

    /* BMS — адрес не задан */
    cfg->bms_addr_set = false;

    /* Charging */
    cfg->soc_start_pct      = CONFIG_SOC_START_DEFAULT;
    cfg->soc_stop_pct       = CONFIG_SOC_STOP_DEFAULT;
    cfg->cell_min_start_mv  = CONFIG_CELL_MIN_START_MV_DEFAULT;
    cfg->cell_max_stop_mv   = CONFIG_CELL_MAX_STOP_MV_DEFAULT;
    cfg->pack_stop_mv       = CONFIG_PACK_STOP_MV_DEFAULT;

    /* GPIO */
    cfg->charger_gpio        = CONFIG_CHARGER_GPIO_DEFAULT;
    cfg->charger_active_high = CONFIG_CHARGER_ACTIVE_HIGH_DEFAULT;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void settings_init(void)
{
    set_defaults(&s_cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved config, using defaults");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed (%s), using defaults", esp_err_to_name(err));
        return;
    }

    load_str  (h, "wifi_ssid",       s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    load_str  (h, "wifi_pass",       s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
    load_str  (h, "auth_user",       s_cfg.auth_user, sizeof(s_cfg.auth_user));
    load_str  (h, "auth_pass",       s_cfg.auth_pass, sizeof(s_cfg.auth_pass));
    load_blob (h, "bms_addr",        s_cfg.bms_addr,  CONFIG_BMS_ADDR_LEN);
    load_u8   (h, "bms_addr_set",    (uint8_t *)&s_cfg.bms_addr_set);
    load_u8   (h, "soc_start",       &s_cfg.soc_start_pct);
    load_u8   (h, "soc_stop",        &s_cfg.soc_stop_pct);
    load_u16  (h, "cell_min_start",  &s_cfg.cell_min_start_mv);
    load_u16  (h, "cell_max_stop",   &s_cfg.cell_max_stop_mv);
    load_u32  (h, "pack_stop",       &s_cfg.pack_stop_mv);
    load_u8   (h, "chg_gpio",        &s_cfg.charger_gpio);
    load_u8   (h, "chg_act_high",    (uint8_t *)&s_cfg.charger_active_high);

    nvs_close(h);

    char soc_start[8], soc_stop[8], cell_start[12], cell_stop[12], pack_stop[16];
    if (s_cfg.soc_start_pct)     snprintf(soc_start,  sizeof(soc_start),  "%u%%",   s_cfg.soc_start_pct);
    else                         snprintf(soc_start,  sizeof(soc_start),  "off");
    if (s_cfg.soc_stop_pct)      snprintf(soc_stop,   sizeof(soc_stop),   "%u%%",   s_cfg.soc_stop_pct);
    else                         snprintf(soc_stop,   sizeof(soc_stop),   "off");
    if (s_cfg.cell_min_start_mv) snprintf(cell_start, sizeof(cell_start), "%umV",   s_cfg.cell_min_start_mv);
    else                         snprintf(cell_start, sizeof(cell_start), "off");
    if (s_cfg.cell_max_stop_mv)  snprintf(cell_stop,  sizeof(cell_stop),  "%umV",   s_cfg.cell_max_stop_mv);
    else                         snprintf(cell_stop,  sizeof(cell_stop),  "off");
    if (s_cfg.pack_stop_mv)      snprintf(pack_stop,  sizeof(pack_stop),  "%lumV",  (unsigned long)s_cfg.pack_stop_mv);
    else                         snprintf(pack_stop,  sizeof(pack_stop),  "off");

    ESP_LOGI(TAG, "Config loaded: ssid='%s'  soc start=%s stop=%s  cell start=%s stop=%s  pack stop=%s",
             s_cfg.wifi_ssid, soc_start, soc_stop, cell_start, cell_stop, pack_stop);
}

const app_config_t *settings_get(void)
{
    return &s_cfg;
}

void settings_save(const app_config_t *cfg)
{
    s_cfg = *cfg;   /* обновить копию в RAM */

    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));

    nvs_set_str  (h, "wifi_ssid",      cfg->wifi_ssid);
    nvs_set_str  (h, "wifi_pass",      cfg->wifi_pass);
    nvs_set_str  (h, "auth_user",      cfg->auth_user);
    nvs_set_str  (h, "auth_pass",      cfg->auth_pass);
    nvs_set_blob (h, "bms_addr",       cfg->bms_addr, CONFIG_BMS_ADDR_LEN);
    nvs_set_u8   (h, "bms_addr_set",   cfg->bms_addr_set);
    nvs_set_u8   (h, "soc_start",      cfg->soc_start_pct);
    nvs_set_u8   (h, "soc_stop",       cfg->soc_stop_pct);
    nvs_set_u16  (h, "cell_min_start", cfg->cell_min_start_mv);
    nvs_set_u16  (h, "cell_max_stop",  cfg->cell_max_stop_mv);
    nvs_set_u32  (h, "pack_stop",      cfg->pack_stop_mv);
    nvs_set_u8   (h, "chg_gpio",       cfg->charger_gpio);
    nvs_set_u8   (h, "chg_act_high",   cfg->charger_active_high);

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config saved");
}

void settings_reset(void)
{
    ESP_LOGI(TAG, "Resetting config to defaults");
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    set_defaults(&s_cfg);
}

/* ── Pending WiFi (двухэтапная фиксация) ─────────────────────────────────── */

#define NVS_NS_PEND "wifi_pend"

void settings_save_pending_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_PEND, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Pending WiFi saved: ssid='%s'", ssid);
}

bool settings_load_pending_wifi(char *ssid, size_t ssid_len,
                                char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_PEND, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, "ssid", ssid, &(size_t){ssid_len}) == ESP_OK &&
              nvs_get_str(h, "pass", pass, &(size_t){pass_len}) == ESP_OK;
    nvs_close(h);
    return ok && ssid[0] != '\0';
}

void settings_clear_pending_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_PEND, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Pending WiFi cleared");
}
