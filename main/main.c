#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_coexist.h"
#include "driver/gpio.h"

#include "config.h"
#include "wifi_manager.h"
#include "bms_ble.h"
#include "charge_ctrl.h"
#include "web_server.h"

static const char *TAG = "main";

/*
 * Удерживать BOOT (GPIO9) 3 секунды во время работы прошивки → сброс настроек.
 * Проверка при старте (while holding RESET) невозможна: ESP32 уходит в DL-режим.
 * Поэтому мониторинг ведётся в отдельной задаче непрерывно.
 */
static void boot_monitor_task(void *arg)
{
    int held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += 100;
            if (held_ms >= BOOT_HOLD_MS) {
                ESP_LOGI(TAG, "BOOT held %d ms — resetting all settings", held_ms);
                settings_reset();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
    }
}

void app_main(void)
{
    /* Перехватить лог-вывод в кольцевой буфер как можно раньше */
    web_server_log_init();

    /* Подавить debug-спам системных компонентов, оставить INFO для наших */
    esp_log_level_set("*",            ESP_LOG_WARN);
    esp_log_level_set("main",         ESP_LOG_INFO);
    esp_log_level_set("config",       ESP_LOG_INFO);
    esp_log_level_set("wifi_manager", ESP_LOG_INFO);
    esp_log_level_set("bms_ble",      ESP_LOG_INFO);
    esp_log_level_set("charge_ctrl",  ESP_LOG_INFO);
    esp_log_level_set("web_server",   ESP_LOG_INFO);
    /* Подавить штатные WARN от httpd: 404 favicon и EAGAIN на отправке */
    esp_log_level_set("httpd_uri",    ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx",   ESP_LOG_ERROR);

    /* NVS должен быть инициализирован первым — от него зависят config и WiFi */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Загрузить настройки из NVS (или заводские значения при первом запуске) */
    settings_init();

    /* Настроить GPIO кнопки BOOT и запустить задачу мониторинга */
    gpio_config_t boot_io = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_io);
    xTaskCreate(boot_monitor_task, "boot_btn", 2048, NULL, 3, NULL);

    /* Подключиться к WiFi или поднять AP для первоначальной настройки */
    wifi_manager_init();

    /* Приоритет BT: потеря BMS-данных хуже, чем чуть более медленный веб-интерфейс.
     * Должен вызываться после wifi_manager_init() и до bms_ble_init(). */
    esp_coex_preference_set(ESP_COEX_PREFER_BT);

    /* Запустить BLE клиент для связи с BMS */
    bms_ble_init();

    /* Запустить контроллер зарядки (GPIO + NVS resume) */
    charge_ctrl_init();

    /* Запустить HTTP сервер (мониторинг + настройки) */
    web_server_start();

    if (wifi_manager_is_ap_mode()) {
        ESP_LOGI(TAG, "Setup mode: connect to WiFi 'BMS-Setup-XXXXXX', open 192.168.4.1");
    } else {
        ESP_LOGI(TAG, "Running in STA mode");
    }
}
