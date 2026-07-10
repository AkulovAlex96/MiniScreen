#include "gfx.h"
#include "config.h"

LGFX tft;
LGFX_Sprite spr(&tft);

LGFX::LGFX() {
    {
        auto cfg = _bus.config();
        cfg.spi_host    = SPI2_HOST;
        cfg.spi_mode    = 0;
        // 40 МГц ок: «глюки цвета» на 40 МГц оказались багом типов, не SPI.
        cfg.freq_write  = 40000000;
        cfg.freq_read   = 16000000;
        cfg.spi_3wire   = false;
        cfg.use_lock    = true;
        cfg.dma_channel = SPI_DMA_CH_AUTO;
        cfg.pin_sclk    = 11;
        cfg.pin_mosi    = 12;
        cfg.pin_miso    = -1;
        cfg.pin_dc      = 13;
        _bus.config(cfg);
        _panel.setBus(&_bus);
    }
    {
        auto cfg = _panel.config();
        cfg.pin_cs       = 10;
        cfg.pin_rst      = 14;
        cfg.pin_busy     = -1;
        cfg.panel_width  = 240;
        cfg.panel_height = 240;
        cfg.readable     = false;
        cfg.invert       = true;
        cfg.rgb_order    = false;
        cfg.dlen_16bit   = false;
        cfg.bus_shared   = false;
        _panel.config(cfg);
    }
    {
        auto cfg = _backlight.config();
        cfg.pin_bl      = 21;
        cfg.invert      = false;
        cfg.freq        = 44100;
        cfg.pwm_channel = 7;
        _backlight.config(cfg);
        _panel.setLight(&_backlight);
    }
    setPanel(&_panel);
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

uint16_t hsv565(uint8_t h, uint8_t s, uint8_t v) {
    uint8_t r, g, b;
    if (s == 0) { r = g = b = v; }
    else {
        uint8_t region    = h / 43;
        uint8_t remainder = (h - region * 43) * 6;
        uint8_t p = (v * (255 - s)) >> 8;
        uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
        uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
        switch (region) {
            case 0: r=v; g=t; b=p; break;
            case 1: r=q; g=v; b=p; break;
            case 2: r=p; g=v; b=t; break;
            case 3: r=p; g=q; b=v; break;
            case 4: r=t; g=p; b=v; break;
            default: r=v; g=p; b=q; break;
        }
    }
    return rgb565(r, g, b);
}

void arcSeg(int r0, int r1, float a0, float a1, uint16_t col) {
    if (a1 > 360.0f) {
        spr.fillArc(120, 120, r0, r1, a0, 360.0f, col);
        spr.fillArc(120, 120, r0, r1, 0.0f, a1 - 360.0f, col);
    } else {
        spr.fillArc(120, 120, r0, r1, a0, a1, col);
    }
}

void drawHand(float angRad, float len, float halfW, float tail, uint16_t col) {
    float dx = sinf(angRad), dy = -cosf(angRad);
    float px = -dy, py = dx;
    float tipX = 120 + dx * len,  tipY = 120 + dy * len;
    float bX   = 120 - dx * tail, bY   = 120 - dy * tail;
    spr.fillTriangle((int)tipX, (int)tipY,
                     (int)(bX + px * halfW), (int)(bY + py * halfW),
                     (int)(bX - px * halfW), (int)(bY - py * halfW), col);
}

// НЕ пушит спрайт: внутри tick() экрана пуш сделает ядро (см. core/screen.h).
// Если статус нужен на экране немедленно (перед блокирующим фетчем или вне
// tick) — явный spr.pushSprite(0,0) после вызова.
void statusScreen(const char* line1, const char* line2, uint16_t col) {
    spr.fillSprite(C_BLACK);
    spr.fillArc(120, 120, 119, 116, 0.0f, 360.0f, col);
    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextDatum(lgfx::middle_center);
    spr.setTextSize(2);
    spr.setTextColor(C_WHITE);
    spr.drawString(line1, 120, 105);
    spr.setTextSize(1);
    spr.setTextColor(C_GREY);
    spr.drawString(line2, 120, 140);
}

namespace gfx {

static const uint8_t BRIGHTNESS_LEVELS[] = { 220, 130, 60, 0 };
static const int     BRIGHTNESS_COUNT = sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);

void init() {
    tft.init();
    tft.setRotation(0);
    tft.setBrightness(220);
    tft.fillScreen(C_BLACK);

    spr.setColorDepth(16);
    spr.setSwapBytes(true);   // строки PNG/палитры GIF приходят little-endian
    spr.setPsram(true);       // фреймбуфер в PSRAM: освобождает 115КБ heap для TLS
    if (!spr.createSprite(240, 240)) {
        spr.setPsram(false);
        if (!spr.createSprite(240, 240)) {
            tft.setTextDatum(lgfx::middle_center);
            tft.setTextColor(C_RED);
            tft.setTextSize(2);
            tft.drawString("NO MEMORY", 120, 120);
            while (true) delay(1000);
        }
    }
}

void applyBrightness() {
    int idx = hubCfg.brightnessIdx;
    if (idx < 0 || idx >= BRIGHTNESS_COUNT) idx = 0;
    tft.setBrightness(BRIGHTNESS_LEVELS[idx]);
}

void cycleBrightness() {
    hubCfg.brightnessIdx = (hubCfg.brightnessIdx + 1) % BRIGHTNESS_COUNT;
    applyBrightness();
    hubSave();
}

} // namespace gfx
