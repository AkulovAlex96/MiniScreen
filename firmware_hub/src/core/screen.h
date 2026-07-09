#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "buttons.h"

// ─── Контракт экрана ──────────────────────────────────────────────────────────
// Экран рисует в общий спрайт spr (core/gfx.h) и САМ вызывает
// spr.pushSprite(0,0), когда кадр готов (экраны с редкой перерисовкой типа
// rain_radar просто не пушат каждый tick). Сетевые запросы — внутри tick()
// по своим таймерам, блокирующий фетч на 1-2 секунды допустим.
//
// Тяжёлые буферы (PSRAM-спрайты и т.п.) выделять в onEnter() и освобождать
// в onExit() — иначе несколько «жирных» экранов не уживутся.

class Screen {
public:
    virtual ~Screen() {}

    virtual const char* id() const = 0;      // "radar" — ключ в NVS и веб-API
    virtual const char* title() const = 0;   // "Flight radar" — для веба/оверлея

    virtual void onEnter() {}
    virtual void onExit()  {}
    virtual void tick(uint32_t nowMs) = 0;
    virtual uint32_t frameDelayMs() const { return 40; }   // pacing менеджера

    // Только BTN1 short/long: BTN2 и тач обрабатывает ядро (яркость/переключение)
    virtual void onButton(BtnEvent ev) { (void)ev; }

    // Настройки экрана <-> веб (генерическая форма из JSON). Экран сам хранит
    // их в своём NVS-неймспейсе внутри applyConfig().
    virtual void getConfig(JsonObject out) { (void)out; }
    virtual void applyConfig(JsonObjectConst in) { (void)in; }

    // Сменился hub-конфиг (координаты дома и т.п.) или контент экрана
    // (веб-загрузка GIF). Вызывается и для неактивных экранов.
    virtual void onConfigChanged() {}
};
