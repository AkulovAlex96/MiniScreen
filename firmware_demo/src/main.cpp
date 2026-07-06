#include <Arduino.h>
#include <LovyanGFX.hpp>

// ─── LGFX: GC9A01 240x240 круглый ────────────────────────────────────────────
// ВАЖНО про цвета в LovyanGFX: тип аргумента определяет формат!
//   uint16_t → RGB565, uint32_t → RGB888.
// Все цвета здесь — строго uint16_t (RGB565).

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_GC9A01 _panel;
    lgfx::Bus_SPI      _bus;
    lgfx::Light_PWM    _backlight;
public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 20000000;
            cfg.freq_read   = 8000000;
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
            cfg.invert       = true;    // GC9A01: INVON обязателен
            cfg.rgb_order    = false;   // дефолт (BGR-панель, MADCTL 0x08)
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
};
static LGFX tft;

#define BTN1_PIN 1   // следующий режим
#define BTN2_PIN 2   // подсветка вкл/выкл

// Цвета RGB565 — строго uint16_t!
static const uint16_t C_BLACK   = 0x0000;
static const uint16_t C_WHITE   = 0xFFFF;
static const uint16_t C_RED     = 0xF800;
static const uint16_t C_GREEN   = 0x07E0;
static const uint16_t C_BLUE    = 0x001F;
static const uint16_t C_YELLOW  = 0xFFE0;
static const uint16_t C_CYAN    = 0x07FF;
static const uint16_t C_MAGENTA = 0xF81F;
static const uint16_t C_ORANGE  = 0xFD20;
static const uint16_t C_GREY    = 0x7BEF;

static uint8_t currentMode = 0;
static const uint8_t NUM_MODES = 3;
static bool blOn = true;

// ─── Утилиты ─────────────────────────────────────────────────────────────────

// HSV → RGB565 (uint16_t!)
uint16_t hsv565(uint8_t h, uint8_t s, uint8_t v) {
    uint8_t r, g, b;
    if (s == 0) {
        r = g = b = v;
    } else {
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
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

bool btn1Pressed() {
    if (!digitalRead(BTN1_PIN)) { delay(20); while (!digitalRead(BTN1_PIN)); return true; }
    return false;
}
bool btn2Pressed() {
    if (!digitalRead(BTN2_PIN)) { delay(20); while (!digitalRead(BTN2_PIN)); return true; }
    return false;
}

void checkButtons() {
    if (btn1Pressed()) {
        currentMode = (currentMode + 1) % NUM_MODES;
        tft.fillScreen(C_BLACK);
    }
    if (btn2Pressed()) {
        blOn = !blOn;
        tft.setBrightness(blOn ? 220 : 0);
    }
}

// ─── Режим 0: Цвета ──────────────────────────────────────────────────────────

struct ColorEntry { uint16_t color; const char* name; uint16_t text; };
static const ColorEntry COLORS[] = {
    { C_RED,     "RED",     C_WHITE },
    { C_GREEN,   "GREEN",   C_BLACK },
    { C_BLUE,    "BLUE",    C_WHITE },
    { C_YELLOW,  "YELLOW",  C_BLACK },
    { C_CYAN,    "CYAN",    C_BLACK },
    { C_MAGENTA, "MAGENTA", C_WHITE },
    { C_WHITE,   "WHITE",   C_BLACK },
    { C_ORANGE,  "ORANGE",  C_BLACK },
};
static const int NUM_COLORS = sizeof(COLORS) / sizeof(COLORS[0]);

static int colorIdx = 0;
static unsigned long colorTimer = 0;

void runColors() {
    if (millis() - colorTimer < 900) return;
    colorTimer = millis();

    auto& c = COLORS[colorIdx];
    tft.fillScreen(c.color);
    tft.setTextDatum(lgfx::middle_center);
    tft.setTextColor(c.text);
    tft.setTextSize(3);
    tft.drawString(c.name, 120, 120);

    colorIdx = (colorIdx + 1) % NUM_COLORS;
}

// ─── Режим 1: Радуга ─────────────────────────────────────────────────────────

static uint8_t rainbowHue = 0;

void runRainbow() {
    tft.startWrite();
    for (int r = 119; r > 0; r--) {
        uint8_t h = rainbowHue + (uint8_t)(r * 2);
        // кольцо толщиной 2px с перекрытием — drawCircle оставлял дырки на диагоналях
        tft.fillArc(120, 120, r + 1, r, 0.0f, 360.0f, hsv565(h, 230, 200));
    }
    tft.fillCircle(120, 120, 1, hsv565(rainbowHue, 230, 200));
    tft.endWrite();
    rainbowHue += 4;
    delay(25);
}

// ─── Режим 2: Текст (в центре круга) ─────────────────────────────────────────

static bool textDrawn = false;

void runText() {
    if (textDrawn) return;
    textDrawn = true;

    tft.fillScreen(C_BLACK);

    tft.fillArc(120, 120, 119, 114, 0.0f, 360.0f, hsv565(150, 220, 200));

    tft.setTextDatum(lgfx::middle_center);

    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.drawString("MiniScreen", 120, 78);

    tft.setTextColor(C_CYAN);
    tft.setTextSize(1);
    tft.drawString("ESP32-S3 + GC9A01", 120, 108);
    tft.drawString("240 x 240", 120, 124);

    tft.setTextColor(C_YELLOW);
    tft.drawString("DEMO v0.1", 120, 144);

    tft.setTextColor(C_GREEN);
    tft.drawString("BTN1: next mode", 120, 166);
    tft.drawString("BTN2: backlight", 120, 180);
}

// ─── Setup / Loop ─────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("GC9A01 demo boot");

    pinMode(BTN1_PIN, INPUT_PULLUP);
    pinMode(BTN2_PIN, INPUT_PULLUP);

    tft.init();
    tft.setRotation(0);
    tft.setBrightness(220);
    tft.fillScreen(C_BLACK);

    // Стартовый экран
    tft.setTextDatum(lgfx::middle_center);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.drawString("GC9A01", 120, 105);
    tft.setTextSize(1);
    tft.setTextColor(C_CYAN);
    tft.drawString("Booting...", 120, 135);
    delay(1000);
    tft.fillScreen(C_BLACK);

    colorTimer = millis();
    Serial.println("Ready. BTN1=mode, BTN2=backlight");
}

void loop() {
    checkButtons();

    if (currentMode != 2) textDrawn = false;

    switch (currentMode) {
        case 0: runColors();  break;
        case 1: runRainbow(); break;
        case 2: runText();    break;
    }
}
