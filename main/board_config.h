#pragma once

/*
 * board_config.h — параметры конкретной платы и чипа.
 *
 * При портировании на другую платформу достаточно изменить только этот файл.
 *
 * Текущая цель: ESP32-C3 (плата с LED на GPIO8, кнопкой BOOT на GPIO9,
 *               подтяжками 10кОм к 3.3В на GPIO2, GPIO8, GPIO9).
 */

/* ── Фиксированные пины платы ──────────────────────────────────────────────── */

/* Встроенный светодиод (active-low: LOW = горит, HIGH = не горит).
 * На ESP32-S3 DevKit — обычно GPIO48. */
#define BOARD_LED_GPIO          8

/* Кнопка BOOT для сброса настроек.
 * ESP32-C3: GPIO9.  ESP32-S3: GPIO0. */
#define BOARD_BOOT_BUTTON_GPIO  9

/* ── Допустимые GPIO для управления зарядником ─────────────────────────────── */

#define BOARD_CHARGER_GPIO_MIN      0
#define BOARD_CHARGER_GPIO_MAX      7
#define BOARD_CHARGER_GPIO_DEFAULT  3

/*
 * Битовая маска GPIO, запрещённых для зарядника (в диапазоне MIN..MAX).
 * GPIO2: подтянут к 3.3В через 10кОм на плате (strapping pin XTAL_32K_N).
 *
 * GPIO8 (LED) и GPIO9 (BOOT) также подтянуты, но уже вне диапазона MAX=7.
 * На ESP32-S3 уточнить strapping pins по схеме платы и добавить их сюда.
 */
#define BOARD_GPIO_FORBIDDEN_MASK   (1ULL << 2)
