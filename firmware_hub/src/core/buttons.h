#pragma once
#include <Arduino.h>

// ─── Кнопки: BTN1/BTN2 (на GND, pull-up) + сенсорная TTP223 (GPIO15) ─────────
// Все опросы неблокирующие. Короткое/долгое нажатие различается по 600мс.
// Маршрутизация (см. main.cpp): BTN2 и тач — глобальные (яркость/переключение
// экрана), BTN1 short/long отдаётся активному экрану.

#define BTN1_PIN  1
#define BTN2_PIN  2
#define TOUCH_PIN 15

enum BtnEvent {
    EV_NONE = 0,
    EV_BTN1_SHORT, EV_BTN1_LONG,
    EV_BTN2_SHORT, EV_BTN2_LONG,
    EV_TOUCH,
};

void buttonsInit();
BtnEvent buttonsPoll();   // не более одного события за вызов
