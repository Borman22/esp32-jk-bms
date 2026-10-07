#include "stats.h"
#include "bms_ble.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "stats";

#define NVS_NAMESPACE  "stats"
#define NVS_KEY_BOOT   "boot"
#define NVS_KEY_HEAD   "head"

#define START_MA        200   /* |ток| выше — кандидат на начало сессии */
#define START_HOLD_S    10
#define END_MA          100   /* |ток| ниже — кандидат на конец сессии */
#define END_HOLD_S      10
#define CHECKPOINT_S    600   /* как часто сохранять активную сессию в NVS */

/*
 * Журнал — кольцо из STATS_MAX слотов. Каждая сессия получает сквозной номер
 * seq (0, 1, 2…) и лежит в слоте seq % STATS_MAX, поэтому 65-я сессия сама
 * затирает 1-ю. s_head — seq СЛЕДУЮЩЕЙ записи, то есть последняя лежит
 * в слоте (s_head − 1) % STATS_MAX. Копия журнала держится в RAM, чтобы веб-запрос
 * не читал 64 блоба из NVS; мьютекс защищает её от задачи stats и HTTP-хендлера
 * (здесь, в отличие от лога, мьютекс уместен — вызовы только из обычных задач).
 */
static SemaphoreHandle_t s_mux;
static stats_rec_t       s_recs[STATS_MAX];
static uint32_t          s_head;
static uint32_t          s_boot;             /* номер текущей загрузки */
static bool              s_unstamped;        /* есть записи этой загрузки без ts_start */

/* ── NVS ──────────────────────────────────────────────────────────────────── */

/* Записать один слот в NVS (ключ "r<слот>"). Вызывать под s_mux. */
static void save_rec(uint32_t slot)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    char key[8];
    snprintf(key, sizeof(key), "r%u", (unsigned)slot);
    nvs_set_blob(h, key, &s_recs[slot], sizeof(stats_rec_t));
    nvs_commit(h);
    nvs_close(h);
}

static void save_u32(const char *key, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static void load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_u32(h, NVS_KEY_BOOT, &s_boot);
    nvs_get_u32(h, NVS_KEY_HEAD, &s_head);
    for (uint32_t slot = 0; slot < STATS_MAX; slot++) {
        char key[8];
        size_t len = sizeof(stats_rec_t);
        snprintf(key, sizeof(key), "r%u", (unsigned)slot);
        if (nvs_get_blob(h, key, &s_recs[slot], &len) != ESP_OK || len != sizeof(stats_rec_t))
            memset(&s_recs[slot], 0, sizeof(stats_rec_t));
    }
    nvs_close(h);
}

/* ── Время ────────────────────────────────────────────────────────────────── */

static uint32_t uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000LL); }

/*
 * Без RTC и до синхронизации time() возвращает время около 1970 года, поэтому
 * «часы установлены» = дата позже ноября 2023. Тот же признак работает для обоих
 * источников времени: SNTP и POST /api/time оба просто двигают системные часы.
 */
bool stats_clock_synced(void) { return time(NULL) > 1700000000; }

/*
 * Проставить абсолютное время записям ТЕКУЩЕЙ загрузки, у которых его не было.
 * Внутри одной загрузки uptime идёт непрерывно, поэтому момент старта сессии
 * восстанавливается как now − (uptime_сейчас − up_start). Записи прошлых загрузок
 * так восстановить нельзя: после перезагрузки неизвестно, сколько устройство
 * простояло выключенным.
 */
static void stamp_current_boot(void)
{
    uint32_t now = (uint32_t)time(NULL), up = uptime_s();
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (uint32_t slot = 0; slot < STATS_MAX; slot++) {
        stats_rec_t *r = &s_recs[slot];
        if (r->type && r->boot == s_boot && r->ts_start == 0) {
            r->ts_start = now - (up - r->up_start);
            save_rec(slot);
        }
    }
    s_unstamped = false;
    xSemaphoreGive(s_mux);
}

/* ── Детектор сессий ──────────────────────────────────────────────────────── */

/* Одна строка в обычный лог при завершении сессии (она же попадёт на вкладку «Логи») */
static void log_session(const char *what, const stats_rec_t *r)
{
    ESP_LOGI(TAG, "%s: %u:%02u:%02u, %.2f -> %.2f Ah (%+.2f Ah)", what,
             (unsigned)(r->dur_s / 3600), (unsigned)(r->dur_s / 60 % 60), (unsigned)(r->dur_s % 60),
             r->mah_start / 1000.0, r->mah_end / 1000.0,
             ((double)r->mah_end - (double)r->mah_start) / 1000.0);
}

/*
 * Детектор сессий, шаг раз в секунду. Два состояния:
 *   active == 0 — сессии нет; ждём, пока ток START_HOLD_S секунд подряд будет
 *                 больше START_MA в одну сторону (cand/cand_s — это счётчик).
 *   active != 0 — сессия идёт; завершаем, когда ток END_HOLD_S секунд подряд
 *                 ниже END_MA (quiet_s — счётчик «тихих» секунд).
 * Пороги начала и конца разные (гистерезис), иначе ток около порога дробил бы
 * одну сессию на множество коротких.
 */
static void stats_task(void *arg)
{
    uint8_t  cand = 0;          /* кандидат на начало: 0 нет, иначе STATS_TYPE_* */
    uint32_t cand_s = 0;        /* сколько секунд подряд держится кандидат */
    uint8_t  active = 0;        /* тип идущей сессии, 0 — нет */
    uint32_t quiet_s = 0;       /* сколько секунд подряд ток ниже порога конца */
    uint32_t slot = 0;          /* слот идущей сессии */
    uint32_t last_act_up = 0;   /* uptime последней секунды, когда ток ещё шёл */
    uint32_t last_act_mah = 0;  /* cap_remain в тот же момент */
    uint32_t last_ckpt = 0;     /* uptime последнего сохранения в NVS */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* Время могло появиться (SNTP или браузер) — проставить его накопленным записям */
        if (s_unstamped && stats_clock_synced()) stamp_current_boot();

        bms_data_t d;
        bms_get_data(&d);
        /* Нет связи с BMS — данных нет, счётчики не трогаем: сессия не прерывается */
        if (!d.valid) continue;
        uint32_t up = uptime_s();
        int32_t  i  = d.current;

        if (!active) {
            /* Знак тока — направление; смена направления обнуляет счётчик кандидата */
            uint8_t want = i > START_MA ? STATS_TYPE_CHARGE : (i < -START_MA ? STATS_TYPE_DISCHARGE : 0);
            if (want != cand) { cand = want; cand_s = 0; }
            if (!cand || ++cand_s < START_HOLD_S) continue;

            /* Начало подтверждено. Сессия началась START_HOLD_S секунд назад, их и вычитаем */
            active = cand; cand = 0; cand_s = 0; quiet_s = 0;
            xSemaphoreTake(s_mux, portMAX_DELAY);
            slot = s_head % STATS_MAX;
            stats_rec_t *r = &s_recs[slot];
            memset(r, 0, sizeof(*r));
            r->type      = active;
            r->state     = STATS_STATE_ACTIVE;
            r->boot      = s_boot;
            r->up_start  = up > START_HOLD_S ? up - START_HOLD_S : 0;
            r->mah_start = r->mah_end = d.cap_remain;
            if (stats_clock_synced()) r->ts_start = (uint32_t)time(NULL) - START_HOLD_S;
            else                      s_unstamped = true;
            /* Сначала запись, потом head: при сбое между ними запись просто будет
               перезаписана следующей сессией, а журнал останется согласованным */
            save_rec(slot);
            s_head++;
            save_u32(NVS_KEY_HEAD, s_head);
            xSemaphoreGive(s_mux);
            last_act_up = up; last_act_mah = d.cap_remain; last_ckpt = up;
            ESP_LOGI(TAG, "%s started at %.2f Ah",
                     active == STATS_TYPE_CHARGE ? "Charge" : "Discharge", d.cap_remain / 1000.0);
            continue;
        }

        /* Идёт ли ток в направлении сессии выше порога конца */
        bool flowing = (active == STATS_TYPE_CHARGE) ? (i > END_MA) : (i < -END_MA);
        if (flowing) {
            quiet_s = 0;
            last_act_up = up; last_act_mah = d.cap_remain;
            /* RAM обновляем каждую секунду (вкладка видит живую сессию), в NVS — редко */
            xSemaphoreTake(s_mux, portMAX_DELAY);
            s_recs[slot].dur_s   = last_act_up - s_recs[slot].up_start;
            s_recs[slot].mah_end = last_act_mah;
            xSemaphoreGive(s_mux);
        } else {
            quiet_s++;
        }

        /* Конец сессии = момент ПОСЛЕДНЕГО тока, а не момент, когда мы это поняли:
           dur_s и mah_end остаются от last_act_*, «тихие» END_HOLD_S секунд не считаются */
        bool ending = quiet_s >= END_HOLD_S;
        if (ending || up - last_ckpt >= CHECKPOINT_S) {
            xSemaphoreTake(s_mux, portMAX_DELAY);
            stats_rec_t *r = &s_recs[slot];
            if (ending) r->state = STATS_STATE_DONE;
            save_rec(slot);  /* чекпоинт: если питание пропадёт, потеряем не больше CHECKPOINT_S */
            xSemaphoreGive(s_mux);
            last_ckpt = up;
            if (ending) {
                log_session(active == STATS_TYPE_CHARGE ? "Charge ended" : "Discharge ended", &s_recs[slot]);
                active = 0;
            }
        }
    }
}

/* ── API ──────────────────────────────────────────────────────────────────── */

/* Копия журнала для веба: новые первыми, пустые слоты (type == 0) пропускаются */
int stats_snapshot(stats_rec_t *out, uint32_t *boot_id)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (boot_id) *boot_id = s_boot;
    uint32_t n = s_head < STATS_MAX ? s_head : STATS_MAX;
    int cnt = 0;
    for (uint32_t k = 0; k < n; k++) {
        const stats_rec_t *r = &s_recs[(s_head - 1 - k) % STATS_MAX];
        if (r->type) out[cnt++] = *r;
    }
    xSemaphoreGive(s_mux);
    return cnt;
}

void stats_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    load_all();

    s_boot++;
    save_u32(NVS_KEY_BOOT, s_boot);

    /*
     * Если последняя запись всё ещё «идёт», значит прошлая загрузка оборвалась
     * посреди сессии (пропало питание). Закрыть её штатно было некому — помечаем.
     * Открытой может быть только последняя запись: следующая не начинается,
     * пока не закончится предыдущая.
     */
    if (s_head > 0) {
        uint32_t slot = (s_head - 1) % STATS_MAX;
        if (s_recs[slot].type && s_recs[slot].state == STATS_STATE_ACTIVE) {
            s_recs[slot].state = STATS_STATE_INTERRUPTED;
            save_rec(slot);
            ESP_LOGW(TAG, "Previous session was interrupted by power loss");
        }
    }

    /*
     * SNTP: время из интернета, если он есть. В режиме точки доступа без сети
     * запросы просто не доходят — это не ошибка, время тогда придёт из браузера.
     */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);

    xTaskCreate(stats_task, "stats", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "Boot #%u, %u sessions in journal", (unsigned)s_boot,
             (unsigned)(s_head < STATS_MAX ? s_head : STATS_MAX));
}
