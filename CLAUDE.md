# BMS Web Monitor — контекст для Claude Code

## Что это

ESP32-C3 + JK-BMS (8-канальная LiFePO4) + веб-интерфейс.  
Устройство подключается к BMS по BLE, читает данные и отдаёт их через HTTP/SSE.  
Управляет зарядкой через GPIO-реле по заданным порогам SoC / напряжения ячеек.

## Сборка

```bash
source ~/esp-idf/export.sh   # ESP-IDF v6.0
cd /home/borman/Microcontrollers/ESP32/BMS_WEB
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

HTML встроен в прошивку через `EMBED_FILES` в CMakeLists.txt — при изменении `index.html` нужна полная пересборка и перепрошивка.

## Архитектура

```
main.c → config.c       NVS-настройки + pending WiFi
       → wifi_manager.c  WiFi: STA / AP / APSTA + reconnect
       → bms_ble.c       BLE GATT-клиент JK-BMS
       → charge_ctrl.c   GPIO реле + автозарядка
       → web_server.c    HTTP + SSE + кольцевой лог
            └ index.html  SPA (3 вкладки: Мониторинг, Настройки, Логи)
```

**Порядок инициализации в app_main важен:**
1. `web_server_log_init()` — первым, перехватывает всё что будет после
2. `nvs_flash_init()` → `settings_init()`
3. GPIO9 + `boot_monitor_task`
4. `wifi_manager_init()` — блокирует до подключения/таймаута
5. `esp_coex_preference_set(ESP_COEX_PREFER_BT)` — BT выигрывает у WiFi
6. `bms_ble_init()` → `charge_ctrl_init()` → `web_server_start()`

---

## Критичные нюансы (обязательно читать перед правками)

### GPIO9 — нельзя проверять при старте
GPIO9 — strapping pin ESP32-C3. Если LOW при RESET → чип уходит в DL-режим (загрузчик), прошивка не запускается. Поэтому BOOT-кнопка **не проверяется в начале app_main**. Используется `boot_monitor_task` — поллинг каждые 100 мс во время работы. Удержание 3 с → `settings_reset()` + `esp_restart()`.

### BMS протокол — обязательная последовательность
После GATT-подключения к JK-BMS:
1. Записать `CMD_INIT` (20 байт) в характеристику `0xFFE1`
2. Подождать 200 мс
3. Записать `CMD_GET_INFO` (20 байт)
4. Подождать 500 мс
5. Записать `0x0100` в CCCD (дескриптор `0xFFE1`)

Без `CMD_GET_INFO` стриминг останавливается примерно через 50 фреймов. Без CCCD notify вообще не приходят.  
Фреймы определяются по заголовку `55 AA EB 90`, накапливаются в буфере 512 байт, парсятся при появлении следующего заголовка.

### Ячейки с нулевым напряжением = не подключены
`cell_voltage[i] == 0` → ячейка отсутствует. В логике зарядки (`should_start`, `should_stop`) такие ячейки пропускаются. В веб-интерфейсе не отображаются. Новые ячейки появятся автоматически.

### Лог-буфер — spinlock, не мьютекс
`web_server.c` использует `portMUX_TYPE` + `portENTER_CRITICAL` для кольцевого буфера логов. Намеренно: обработчик логов вызывается из любого контекста включая Bluedroid callbacks, где мьютекс FreeRTOS вызвал бы дедлок. Не менять на мьютекс.

### BLE + WiFi коэксистенция
Единственный радиомодуль. `esp_coex_preference_set(ESP_COEX_PREFER_BT)` — BT получает приоритет. SSE отправляется каждые 500 мс (~700 байт), это незначительно для коэксистенции.

---

## WiFi — режимы и логика

**Логика выбора режима при старте:**
```
Pending WiFi (wifi_pend в NVS)?
  ДА → пробуем; успех → promote в app_cfg; провал → clear pending
Сохранённый SSID?
  НЕТ → prescan + AP-режим
  ДА  → start_sta (15 с, 3 попытки MAX_RETRY)
         успех → STA-режим
         провал → prescan + APSTA-режим (AP + фоновый STA)
```

**Pending WiFi (двухэтапная фиксация):**  
При сохранении новой сети через веб: credentials пишутся в NVS-namespace `"wifi_pend"`, основные `app_cfg` не меняются. После перезагрузки pending пробуется первым. Успех → promote + clear. Провал → clear, подключаемся к старой. Защита от потери доступа при неверном пароле.

**APSTA-режим:**  
Если STA не подключилась (роутер офлайн) но credentials есть → `WIFI_MODE_APSTA`. AP (`BMS-Setup-XXXXXX`) доступен локально, STA в фоне пробует роутер бесконечно (`s_sta_bg_reconnect = true`). Канал AP = канал STA (ограничение железа).

**Бесконечный reconnect:**  
`s_ever_connected = true` после первого IP. При разрыве → `esp_wifi_connect()` без лимита.  
`s_sta_bg_reconnect = true` в APSTA → то же самое для фонового STA.  
`start_sta()` сбрасывает `s_retry = 0` и event bits в начале — без этого второй вызов видит старый `WIFI_FAIL_BIT` и сразу возвращает false.

---

## Логика зарядки

**START** (любое из, только если хотя бы один порог ненулевой):
- `soc <= soc_start_pct`
- любая ненулевая ячейка `< cell_min_start_mv`

**STOP** (любое из):
- `soc >= soc_stop_pct`
- `battery_voltage >= pack_stop_mv`
- любая ненулевая ячейка `>= cell_max_stop_mv`

**NVS resume:** флаг `in_progress` в namespace `"charge"`. При старте если `in_progress=1` → зарядка возобновляется без проверки условий START. Это намеренно: прерванный цикл должен завершиться.

**Важно:** `any_start_configured()` проверяется только в `CHARGE_STATE_IDLE`. В `CHARGE_STATE_CHARGING` — нет. Иначе очистка порогов пользователем остановила бы возобновлённую после питания зарядку.

**Уведомление о смене настроек:** `charge_ctrl_notify_settings_changed()` → `xTaskNotifyGive` → задача просыпается досрочно (не ждёт 5 с).

---

## SSE и HTTP

- До 4 одновременных SSE-клиентов (`SSE_MAX_CLIENTS`). Вытеснение по FIFO (слот 0 — самый старый).
- Обновление каждые 500 мс (в такт с BMS).
- SSE-соединение тоже требует авторизации. Браузер отправляет кэшированные Basic Auth credentials автоматически.
- HTTP-сервер: 7 сокетов, LRU purge, 12 URI-хендлеров.
- Async handler pattern: `sse_handler` возвращается немедленно, `sse_task` шлёт данные.

---

## Авторизация

HTTP Basic Auth. Включается/выключается через настройки, хранится в `app_config_t` (NVS `"app_cfg"`).

- `auth_pass` пустой → авторизация отключена, все запросы пропускаются
- `auth_user` пустой → `check_auth()` подставляет `"admin"`. Пустой логин и логин `"admin"` **равнозначны**
- Все эндпоинты кроме `/favicon.ico` защищены
- `GET /api/settings` возвращает `auth_enabled` (bool) и `auth_user` (строка), **не** возвращает `auth_pass`
- `POST /api/settings` с `auth_enabled=false` → очищает оба поля; с `auth_enabled=true` и непустым `auth_pass` → обновляет пароль; с пустым `auth_pass` → оставляет пароль без изменений
- Изменения вступают в силу немедленно (без перезагрузки)
- Забыт пароль → BOOT 3 с → сброс всех настроек

---

## OTA (обновление прошивки)

Endpoint `POST /api/ota` принимает `application/octet-stream` (бинарный `.bin`).

- Проверяет magic-байт `0xE9` в начале для быстрого отклонения
- Записывает в `esp_ota_get_next_update_partition()` (чередует ota_0/ota_1)
- `esp_ota_end()` верифицирует SHA256 — если провал, boot partition не меняется
- При успехе: `esp_ota_set_boot_partition()` + `esp_restart()` через 500 мс
- Требует авторизации если включена

**Partition layout (4 МБ):**
```
nvs      0x9000   0x5000  (20 КБ, NVS настройки)
otadata  0xE000   0x2000  (8 КБ, активный слот)
ota_0    0x10000  0x1F0000 (1.9375 МБ)
ota_1    0x200000 0x1F0000 (1.9375 МБ)
```
Текущая прошивка ~1.4 МБ → запас ~560 КБ (30%). Таблицу разделов менять не нужно при росте функциональности в пределах этого запаса.

NVS на том же адресе 0x9000 что и в factory-схеме — настройки переживают переход.

---

## NVS-пространства имён

| Namespace   | Что хранит |
|-------------|------------|
| `"app_cfg"` | Все настройки (`app_config_t`), включая `auth_user` и `auth_pass` |
| `"wifi_pend"` | Pending WiFi (ssid + pass, временно) |
| `"charge"`  | `in_progress` (u8) |

---

## HTTP API

| Метод | Путь | Описание |
|-------|------|----------|
| GET | `/` | index.html |
| GET | `/api/data` | Снимок BMS JSON |
| GET | `/api/events` | SSE-поток (500 мс) |
| GET | `/api/settings` | Текущие настройки (пароли не возвращаются; `auth_user` возвращается) |
| POST | `/api/settings` | Сохранить настройки; если WiFi изменился → pending + `restart_required:true` |
| POST | `/api/restart` | Перезагрузка через 500 мс |
| GET | `/api/wifi/scan` | Кэш WiFi-сетей (сортировка по RSSI) |
| GET | `/api/ble/scan` | Замеченные BLE-устройства (JK-BMS первыми) |
| GET | `/api/logs?from=N` | Логи с порядкового номера N; возвращает `{logs:[{seq,msg}], next}` |
| POST | `/api/charger` | `{"on":true/false}` — ручное управление |
| POST | `/api/ota`     | Бинарный образ прошивки → OTA + reboot |

---

## Стиль кода

- Комментарии на русском языке
- Без лишних комментариев — только когда WHY неочевиден
- Без абстракций "на будущее" — только то, что нужно сейчас
- Валидация только на границах системы (HTTP-вход, NVS-чтение)
- Не добавлять обработку ошибок для невозможных сценариев
