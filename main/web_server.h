#pragma once

/*
 * web_server.h — HTTP сервер: мониторинг и настройки через браузер.
 *
 * Эндпоинты:
 *   GET  /               → index.html (веб-интерфейс)
 *   GET  /api/data        → снимок данных BMS в JSON (одиночный запрос)
 *   GET  /api/events      → SSE-поток: данные BMS каждые 2 секунды
 *   GET  /api/settings    → текущие настройки в JSON
 *   POST /api/settings    → сохранить новые настройки из JSON-тела
 *   POST /api/restart     → перезагрузить устройство
 */

/**
 * Инициализировать кольцевой буфер логов и перехватить вывод ESP_LOG*.
 * Должна вызываться как можно раньше в app_main, до всех остальных init().
 * После этого все ESP_LOG* сообщения будут доступны через GET /api/logs.
 */
void web_server_log_init(void);

/**
 * Запустить HTTP сервер.
 * Должна вызываться после wifi_manager_init().
 */
void web_server_start(void);
