#include "bms_ble.h"
#include "config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_defs.h"

static const char *TAG = "bms_ble";

#define BUFFER_SIZE     512
#define BMS_SVC_UUID16  0xffe0
#define BMS_CHR_UUID16  0xffe1
#define PROFILE_APP_ID  0

/*
 * Команды инициализации JK-BMS, захвачены из официального приложения.
 * CMD_INIT     — первичная инициализация, без неё BMS игнорирует последующие команды.
 * CMD_GET_INFO — запрос потоковых данных; без него стриминг останавливается ~через 50 фреймов.
 * Обе команды пишутся в характеристику FFE1 до записи CCCD.
 */
static const uint8_t CMD_INIT[20]     = {0xaa,0x55,0x90,0xeb,0x97,0x00,0xdf,0x52,0x88,0x67,0x9d,0x0a,0x09,0x6b,0x9a,0xf6,0x70,0x9a,0x17,0xfd};
static const uint8_t CMD_GET_INFO[20] = {0xaa,0x55,0x90,0xeb,0x96,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10};

/* Состояние GATT-соединения. Меняется в callback-ах Bluedroid, читается из cmd_task. */
static esp_gatt_if_t       s_gattc_if         = ESP_GATT_IF_NONE;
static uint16_t            s_conn_id          = 0;
static esp_bd_addr_t       s_remote_bda;
static esp_ble_addr_type_t s_remote_addr_type = BLE_ADDR_TYPE_PUBLIC;
static uint16_t            s_svc_start        = 0;
static uint16_t            s_svc_end          = 0;
static uint16_t            s_chr_handle       = 0;
static uint16_t            s_cccd_handle      = 0;
static bool                s_connecting       = false;  /* open отправлен, ответа ещё нет — не открывать повторно */
static bool                s_connected        = false;

/* Фрейм JK-BMS приходит несколькими notify-пакетами; здесь он накапливается целиком */
static uint8_t  s_buf[BUFFER_SIZE];
static size_t   s_buf_idx     = 0;
static uint32_t s_parse_count = 0;  /* успешно разобранных фреймов — по нему работает watchdog */
static uint32_t s_notif_count = 0;  /* принятых notify-пакетов (только для отладки) */

/* Последние данные BMS и список замеченных устройств; оба под одним мьютексом.
 * Мьютекс здесь допустим: callback-и Bluedroid и читатели — обычные задачи. */
static bms_data_t        s_data  = {0};
static SemaphoreHandle_t s_mutex;

static ble_seen_t s_seen[BLE_SEEN_MAX];
static int        s_seen_count = 0;

/* Пассивный скан: мы только слушаем рекламу, запросов не шлём — достаточно, чтобы
 * найти BMS по MAC и заполнить список устройств для выбора в настройках.
 * Окно 80 мс из интервала 100 мс оставляет радио время для WiFi. */
static esp_ble_scan_params_t s_scan_params = {
    .scan_type          = BLE_SCAN_TYPE_PASSIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = 0x00A0,
    .scan_window        = 0x0050,
    .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
};

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                 esp_ble_gattc_cb_param_t *param);
static void cmd_task(void *arg);

// ─── Buffer parsing ───────────────────────────────────────────────────────────

/*
 * Раскладка фрейма JK-BMS (смещения в байтах, little-endian если не указано иное).
 * Смещения получены реверс-инжинирингом захваченных пакетов BLE.
 *
 *  6        — cell_voltage[0..7]  (2 байта каждый, шаг 2)
 *  74       — avg_voltage          (2 байта)
 *  80       — cell_resistance[0..7] (2 байта каждый, шаг 2)
 *  150      — battery_voltage      (2 байта)
 *  154      — power                (4 байта, signed)
 *  158      — current              (4 байта, signed)
 *  162/164  — temp1 / temp2        (2 байта, 0.1°C)
 *  173      — soc                  (1 байт, %)
 *  174/178  — cap_remain / cap_full (4 байта, mAh)
 *  254      — temp_mos             (2 байта, 0.1°C)
 */
static void parse_buffer(void)
{
    /* Фрейм короче 256 байт неполный: последнее нужное поле (temp_mos) лежит по смещению 254 */
    if (s_buf_idx < 256) return;

    const uint8_t *b = s_buf;
    bms_data_t d = {0};

    for (int i = 0; i < 8; i++) {
        d.cell_voltage[i]    = b[6  + i * 2] | (b[7  + i * 2] << 8);
        d.cell_resistance[i] = b[80 + i * 2] | (b[81 + i * 2] << 8);
    }

    d.avg_voltage     = b[74]  | (b[75]  << 8);
    d.battery_voltage = b[150] | (b[151] << 8);
    d.soc             = b[173];
    d.temp1           = b[162] | (b[163] << 8);
    d.temp2           = b[164] | (b[165] << 8);
    d.temp_mos        = b[254] | (b[255] << 8);
    d.cap_remain = (uint32_t)b[174] | ((uint32_t)b[175] << 8) | ((uint32_t)b[176] << 16) | ((uint32_t)b[177] << 24);
    d.cap_full   = (uint32_t)b[178] | ((uint32_t)b[179] << 8) | ((uint32_t)b[180] << 16) | ((uint32_t)b[181] << 24);
    d.power      = (int32_t)((uint32_t)b[154] | ((uint32_t)b[155] << 8) | ((uint32_t)b[156] << 16) | ((uint32_t)b[157] << 24));
    d.current    = (int32_t)((uint32_t)b[158] | ((uint32_t)b[159] << 8) | ((uint32_t)b[160] << 16) | ((uint32_t)b[161] << 24));
    /* BMS иногда кодирует знак тока и мощности независимо — выравниваем */
    if (d.current < 0 && d.power > 0) d.power = -d.power;

    /* Диапазон 1–60 В охватывает все продуктовые линейки JK-BMS; вне диапазона — мусор */
    if (d.battery_voltage < 1000 || d.battery_voltage > 60000) return;
    d.valid = true;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_data = d;
    xSemaphoreGive(s_mutex);

    ++s_parse_count;
}

/*
 * Фрейм не имеет признака конца, поэтому его граница определяется по началу
 * следующего: увидели заголовок → разбираем то, что накопили, и начинаем заново.
 * Последствие: данные обновляются с задержкой в один фрейм (~500 мс).
 */
static void handle_notification(const uint8_t *data, uint16_t len)
{
    /* 55 AA EB 90 — сигнатура начала нового фрейма JK-BMS.
     * При её появлении парсим накопленный предыдущий фрейм и сбрасываем буфер. */
    if (len >= 4 && data[0] == 0x55 && data[1] == 0xAA &&
        data[2] == 0xEB && data[3] == 0x90) {
        if (s_buf_idx > 0) parse_buffer();
        s_buf_idx = 0;
    }
    if (s_buf_idx + len <= BUFFER_SIZE) {
        memcpy(s_buf + s_buf_idx, data, len);
        s_buf_idx += len;
    } else {
        ESP_LOGW(TAG, "Buffer overflow, dropping fragment");
    }
}

// ─── Init commands + watchdog task ───────────────────────────────────────────

/*
 * Запускается после регистрации notify. Выполняет обязательную последовательность
 * из CLAUDE.md (INIT → GET_INFO → CCCD), затем остаётся сторожем потока данных.
 * После каждой паузы проверяем s_connected: если связь пропала, пока мы ждали,
 * писать в закрытое соединение нельзя — задача тихо завершается.
 */
static void cmd_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(200)); /* BMS нужно время обработать подключение */
    if (!s_connected) { vTaskDelete(NULL); return; }

    esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_chr_handle,
                              sizeof(CMD_INIT), (uint8_t *)CMD_INIT,
                              ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);

    vTaskDelay(pdMS_TO_TICKS(500)); /* ждём подтверждения CMD_INIT перед следующей командой */
    if (!s_connected) { vTaskDelete(NULL); return; }

    esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_chr_handle,
                              sizeof(CMD_GET_INFO), (uint8_t *)CMD_GET_INFO,
                              ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);

    vTaskDelay(pdMS_TO_TICKS(1000)); /* ждём пока BMS начнёт стриминг перед записью CCCD */
    if (!s_connected) { vTaskDelete(NULL); return; }

    if (s_cccd_handle != 0) {
        uint8_t notify_en[] = {0x01, 0x00};
        ESP_LOGI(TAG, "Writing CCCD (enable notify)");
        esp_ble_gattc_write_char_descr(s_gattc_if, s_conn_id, s_cccd_handle,
                                       sizeof(notify_en), notify_en,
                                       ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    }

    /* Watchdog: BMS иногда молча перестаёт слать данные при живом соединении.
       Если за 5 с не разобрано ни одного нового фрейма — рвём связь; DISCONNECT_EVT
       запустит скан и переподключение. */
    uint32_t last_count = s_parse_count;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!s_connected) break;

        if (s_parse_count == last_count) {
            ESP_LOGW(TAG, "Stream stopped after %lu frames, reconnecting",
                     (unsigned long)s_parse_count);
            esp_ble_gattc_close(s_gattc_if, s_conn_id);
            break;
        }
        ESP_LOGD(TAG, "Watchdog: notif=%lu parse=%lu",
                 (unsigned long)s_notif_count, (unsigned long)s_parse_count);
        last_count = s_parse_count;
    }
    vTaskDelete(NULL);
}

// ─── Seen device tracking ─────────────────────────────────────────────────────

/*
 * Разбор рекламного пакета: он состоит из блоков [длина][тип][данные].
 * Достаём имя (типы 0x08/0x09) и признак JK-BMS — сервис FFE0 в списке 16-битных
 * UUID (0x02/0x03) либо имя на «JK-». Нужно только для подсказки в интерфейсе,
 * подключение выполняется по MAC из настроек.
 */
static void parse_adv_data(const uint8_t *adv, uint8_t adv_len,
                            char *name_out, bool *is_jk_out)
{
    *is_jk_out = false;
    name_out[0] = '\0';

    for (int i = 0; i < adv_len; ) {
        uint8_t len  = adv[i];
        if (len == 0 || i + len >= adv_len) break;
        uint8_t type = adv[i + 1];

        if (type == 0x08 || type == 0x09) {
            /* Local Name */
            int nlen = len - 1;
            if (nlen > 31) nlen = 31;
            memcpy(name_out, &adv[i + 2], nlen);
            name_out[nlen] = '\0';
        } else if (type == 0x02 || type == 0x03) {
            /* 16-bit Service UUID list */
            for (int j = 0; j + 1 < len - 1; j += 2) {
                uint16_t uuid = adv[i + 2 + j] | ((uint16_t)adv[i + 3 + j] << 8);
                if (uuid == 0xFFE0) *is_jk_out = true;
            }
        }
        i += len + 1;
    }

    /* Дополнительно: имя начинается с "JK-" */
    if (!*is_jk_out && strncmp(name_out, "JK-", 3) == 0)
        *is_jk_out = true;
}

/* Добавить или обновить устройство в списке замеченных (ключ — MAC). При переполнении новые игнорируются. */
static void seen_update(const esp_ble_gap_cb_param_t *p)
{
    const uint8_t *bda = p->scan_rst.bda;
    char addr[18];
    snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

    char  name[32];
    bool  is_jk;
    parse_adv_data(p->scan_rst.ble_adv, p->scan_rst.adv_data_len, name, &is_jk);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Найти уже существующую запись по адресу */
    for (int i = 0; i < s_seen_count; i++) {
        if (strcmp(s_seen[i].addr, addr) == 0) {
            s_seen[i].rssi = p->scan_rst.rssi;
            if (name[0]) strlcpy(s_seen[i].name, name, sizeof(s_seen[i].name));
            if (is_jk) s_seen[i].is_jk_bms = true;
            xSemaphoreGive(s_mutex);
            return;
        }
    }

    /* Новое устройство */
    if (s_seen_count < BLE_SEEN_MAX) {
        ble_seen_t *e = &s_seen[s_seen_count++];
        strlcpy(e->addr, addr, sizeof(e->addr));
        strlcpy(e->name, name, sizeof(e->name));
        e->rssi      = p->scan_rst.rssi;
        e->is_jk_bms = is_jk;
    }

    xSemaphoreGive(s_mutex);
}

// ─── GAP event handler ────────────────────────────────────────────────────────

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Scanning for BMS...");
        esp_ble_gap_start_scanning(0);
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            seen_update(param);
            /* Нашли устройство с MAC из настроек: останавливаем скан и подключаемся.
               s_connecting защищает от повторного open, пока приходят новые пакеты рекламы. */
            const app_config_t *cfg = settings_get();
            if (!s_connecting && cfg->bms_addr_set &&
                memcmp(param->scan_rst.bda, cfg->bms_addr, sizeof(esp_bd_addr_t)) == 0) {
                s_connecting = true;
                esp_ble_gap_stop_scanning();
                memcpy(s_remote_bda, param->scan_rst.bda, sizeof(esp_bd_addr_t));
                s_remote_addr_type = param->scan_rst.ble_addr_type;
                ESP_LOGI(TAG, "Found BMS, connecting...");
                esp_ble_gattc_open(s_gattc_if, s_remote_bda, s_remote_addr_type, true);
            }
        }
        break;

    default:
        break;
    }
}

// ─── GATTC event handler ──────────────────────────────────────────────────────

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                  esp_ble_gattc_cb_param_t *param)
{
    switch (event) {

    /*
     * Цепочка подключения (каждый шаг — отдельное событие):
     * REG → скан → OPEN → MTU → поиск сервиса FFE0 → поиск характеристики FFE1 и её CCCD
     * → register_for_notify → cmd_task. Любая неудача закрывает соединение,
     * и DISCONNECT_EVT снова запускает скан.
     */
    case ESP_GATTC_REG_EVT:
        s_gattc_if = gattc_if;
        esp_ble_gap_set_scan_params(&s_scan_params);
        break;

    case ESP_GATTC_OPEN_EVT:
        s_connecting = false;
        /* Подключиться не удалось — возвращаемся к сканированию */
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "Connect failed (%d), scanning again", param->open.status);
            esp_ble_gap_set_scan_params(&s_scan_params);
            break;
        }
        s_conn_id   = param->open.conn_id;
        s_connected = true;
        ESP_LOGI(TAG, "Connected to BMS");
        esp_ble_gattc_send_mtu_req(gattc_if, s_conn_id);
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        ESP_LOGI(TAG, "MTU=%u", param->cfg_mtu.mtu);
        {
            esp_bt_uuid_t svc_uuid = {
                .len = ESP_UUID_LEN_16,
                .uuid = { .uuid16 = BMS_SVC_UUID16 },
            };
            esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, &svc_uuid);
        }
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            param->search_res.srvc_id.uuid.uuid.uuid16 == BMS_SVC_UUID16) {
            s_svc_start = param->search_res.start_handle;
            s_svc_end   = param->search_res.end_handle;
            ESP_LOGI(TAG, "FFE0 svc 0x%04x-0x%04x", s_svc_start, s_svc_end);
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK || s_svc_start == 0) {
            ESP_LOGE(TAG, "FFE0 service not found");
            esp_ble_gattc_close(gattc_if, param->search_cmpl.conn_id);
            break;
        }
        {
            esp_bt_uuid_t chr_uuid = {
                .len = ESP_UUID_LEN_16,
                .uuid = { .uuid16 = BMS_CHR_UUID16 },
            };
            uint16_t count = 1;
            esp_gattc_char_elem_t chr_elem;
            esp_gatt_status_t st = esp_ble_gattc_get_char_by_uuid(
                gattc_if, param->search_cmpl.conn_id,
                s_svc_start, s_svc_end, chr_uuid, &chr_elem, &count);
            if (st == ESP_GATT_OK && count > 0) {
                s_chr_handle = chr_elem.char_handle;
                ESP_LOGI(TAG, "FFE1 chr_handle=0x%04x", s_chr_handle);

                esp_bt_uuid_t cccd_uuid = {
                    .len = ESP_UUID_LEN_16,
                    .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG },
                };
                esp_gattc_descr_elem_t descr_elem;
                uint16_t descr_count = 1;
                if (esp_ble_gattc_get_descr_by_char_handle(gattc_if,
                        param->search_cmpl.conn_id, s_chr_handle,
                        cccd_uuid, &descr_elem, &descr_count) == ESP_GATT_OK
                    && descr_count > 0) {
                    s_cccd_handle = descr_elem.handle;
                    ESP_LOGI(TAG, "CCCD handle=0x%04x", s_cccd_handle);
                } else {
                    s_cccd_handle = 0;
                    ESP_LOGW(TAG, "CCCD not found");
                }

                esp_ble_gattc_register_for_notify(gattc_if, s_remote_bda, s_chr_handle);
            } else {
                ESP_LOGE(TAG, "FFE1 not found (st=%d)", st);
                esp_ble_gattc_close(gattc_if, param->search_cmpl.conn_id);
            }
        }
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        if (param->reg_for_notify.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "Notify registered, starting cmd_task");
            xTaskCreate(cmd_task, "bms_cmd", 2048, NULL, 5, NULL);
        } else {
            ESP_LOGE(TAG, "Register notify failed: %d", param->reg_for_notify.status);
        }
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Write failed: %d", param->write.status);
        }
        break;

    case ESP_GATTC_NOTIFY_EVT:
        if (param->notify.handle == s_chr_handle) {
            s_notif_count++;
            ESP_LOGD(TAG, "notif#%lu len=%u parse=%lu",
                     (unsigned long)s_notif_count,
                     param->notify.value_len,
                     (unsigned long)s_parse_count);
            handle_notification(param->notify.value, param->notify.value_len);
        }
        break;

    /* Любой разрыв: сбросить всё состояние соединения и данные (valid=false, чтобы
       интерфейс и зарядка не опирались на устаревшие значения) и искать BMS заново */
    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(TAG, "Disconnected (reason %d), scanning again",
                 param->disconnect.reason);
        s_connected   = false;
        s_chr_handle  = 0;
        s_cccd_handle = 0;
        s_svc_start   = 0;
        s_svc_end     = 0;
        s_buf_idx     = 0;
        s_notif_count = 0;
        s_parse_count = 0;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_data.valid = false;
        xSemaphoreGive(s_mutex);
        esp_ble_gap_set_scan_params(&s_scan_params);
        break;

    default:
        break;
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

/* Поднять BLE-стек (Bluedroid, только BLE-режим) и зарегистрировать GATT-клиента.
 * Дальше всё идёт по событиям: REG_EVT запускает скан. */
void bms_ble_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(PROFILE_APP_ID));

    ESP_LOGI(TAG, "Bluedroid BLE initialized, waiting for scan...");
}

/* Копия последних данных (потокобезопасно). При отсутствии связи out->valid == false. */
void bms_get_data(bms_data_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_mutex);
}

int bms_get_seen_devices(ble_seen_t *out, int max_count)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_seen_count < max_count ? s_seen_count : max_count;
    memcpy(out, s_seen, n * sizeof(ble_seen_t));
    xSemaphoreGive(s_mutex);
    return n;
}
