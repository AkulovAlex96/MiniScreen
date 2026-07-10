#pragma once
#include <LovyanGFX.hpp>

// ─── Дисплей GC9A01 240x240 (SPI2) + общий полнокадровый спрайт ───────────────
// Конфиг пинов/панели — тот же, что во всех прошивках-прототипах.
// spr — единый фреймбуфер (115 КБ, PSRAM): все экраны рисуют в него и сами
// делают pushSprite в конце кадра.

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_GC9A01 _panel;
    lgfx::Bus_SPI      _bus;
    lgfx::Light_PWM    _backlight;
public:
    LGFX();
};

extern LGFX tft;
extern LGFX_Sprite spr;

// Цвета RGB565 (строго uint16_t! uint32_t LovyanGFX трактует как RGB888)
static const uint16_t C_BLACK  = 0x0000;
static const uint16_t C_WHITE  = 0xFFFF;
static const uint16_t C_RED    = 0xF800;
static const uint16_t C_GREEN  = 0x07E0;
static const uint16_t C_CYAN   = 0x07FF;
static const uint16_t C_YELLOW = 0xFFE0;
static const uint16_t C_GREY   = 0x7BEF;
static const uint16_t C_DGREY  = 0x39E7;
static const uint16_t C_ACCENT = 0x051F;   // ярко-голубой

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);
uint16_t hsv565(uint8_t h, uint8_t s, uint8_t v);

// Дуга с поддержкой перехода через 360° (центр 120,120)
void arcSeg(int r0, int r1, float a0, float a1, uint16_t col);
// Толстая стрелка часов/gauge (треугольник от центра)
void drawHand(float angRad, float len, float halfW, float tail, uint16_t col);
// Статус-экран: две строки текста в цветном кольце. Только рисует в spr,
// НЕ пушит (в tick() экрана пуш делает ядро; вне tick — пушить явно)
void statusScreen(const char* line1, const char* line2, uint16_t col);

namespace gfx {
void init();                 // tft + создание spr (PSRAM, фолбэк в heap)
void applyBrightness();      // выставить яркость из hubCfg.brightnessIdx
void cycleBrightness();      // BTN2 short: 220 -> 130 -> 60 -> 0 -> ...
}
