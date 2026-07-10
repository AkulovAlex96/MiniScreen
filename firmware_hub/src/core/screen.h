#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "buttons.h"

// ─── Контракт экрана ──────────────────────────────────────────────────────────
// Экран рисует в общий спрайт spr (core/gfx.h) и НЕ пушит его сам: tick()
// возвращает true, если кадр обновлён — ядро вкомпонует оверлей (название
// экрана после переключения) и сделает pushSprite одним куском, без моргания.
// Экраны с редкой перерисовкой (rain_radar и т.п.) возвращают false, пока
// кадр не менялся. Сетевые запросы — внутри tick() по своим таймерам,
// блокирующий фетч на 1-2 секунды допустим (перед ним можно показать статус:
// statusScreen(...) + явный spr.pushSprite(0,0)).
//
// Тяжёлые буферы (PSRAM-спрайты и т.п.) выделять в onEnter() и освобождать
// в onExit() — иначе несколько «жирных» экранов не уживутся. Исключение —
// небольшие кэши СЖАТОГО контента (PNG карты/тайла, ~48КБ PSRAM): их можно
// держать постоянно, чтобы при возврате на экран не ходить в сеть заново.

class Screen {
public:
    virtual ~Screen() {}

    virtual const char* id() const = 0;      // "radar" — ключ в NVS и веб-API
    virtual const char* title() const = 0;   // "Flight radar" — для веба/оверлея

    virtual void onEnter() {}
    virtual void onExit()  {}
    virtual bool tick(uint32_t nowMs) = 0;   // true = кадр в spr обновлён, ядро запушит
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
