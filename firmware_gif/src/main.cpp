#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <AnimatedGIF.h>

#include "gif_registry.h"   // сгенерировано tools/convert_gif.py

// ═══ Проигрывание GIF на GC9A01 240x240 ═══════════════════════════════════════
// GIF встроен в flash как массив, декодируется на лету (AnimatedGIF от bitbank2).
// Кадры собираются в полнокадровый спрайт → pushSprite без мерцания.
//
// BTN1 (GPIO1) — пауза/продолжить
// BTN2 (GPIO2) — подсветка вкл/выкл

// ─── LGFX: конфиг панели (тот же, что в firmware_widgets) ────────────────────

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
};

static LGFX tft;
static LGFX_Sprite spr(&tft);
static AnimatedGIF gif;

#define BTN1_PIN 1
#define BTN2_PIN 2

static bool paused = false;
static bool blOn   = true;
static size_t gifIndex = 0;

// Смещение кадра на экране (GIF ровно 240x240 → 0,0)
static int offX = 0, offY = 0;

static void gifDraw(GIFDRAW *pDraw);

static bool openCurrentGif() {
    if (kGifAssetsCount == 0) {
        return false;
    }

    const GifAsset &asset = kGifAssets[gifIndex];
    if (!gif.open((uint8_t*)asset.data, asset.len, gifDraw)) {
        Serial.printf("GIF open error for '%s': %d\n", asset.name, gif.getLastError());
        return false;
    }

    Serial.printf(
        "GIF[%u/%u]: %s (%lu bytes), canvas=%dx%d\n",
        (unsigned)(gifIndex + 1),
        (unsigned)kGifAssetsCount,
        asset.name,
        (unsigned long)asset.len,
        gif.getCanvasWidth(),
        gif.getCanvasHeight()
    );
    offX = (240 - gif.getCanvasWidth())  / 2;
    offY = (240 - gif.getCanvasHeight()) / 2;
    spr.fillSprite((uint16_t)0x0000);
    return true;
}

// ─── Колбэк отрисовки строки GIF ─────────────────────────────────────────────
// Палитра запрошена в RGB565 little-endian → строки пишем в спрайт как есть.

static void gifDraw(GIFDRAW *pDraw) {
    int width = pDraw->iWidth;
    if (pDraw->iX + width > 240) width = 240 - pDraw->iX;
    if (width <= 0) return;

    int y = pDraw->iY + pDraw->y;
    uint8_t  *src     = pDraw->pPixels;
    uint16_t *palette = pDraw->pPalette;
    static uint16_t line[240];

    if (pDraw->ucHasTransparency) {
        // Прозрачные пиксели не трогаем — в спрайте остаётся предыдущий кадр
        uint8_t tr = pDraw->ucTransparent;
        int x = 0;
        while (x < width) {
            while (x < width && src[x] == tr) x++;       // пропуск прозрачных
            int start = x;
            while (x < width && src[x] != tr)            // копим непрозрачный кусок
                line[x - start] = palette[src[x]], x++;
            if (x > start)
                spr.pushImage(offX + pDraw->iX + start, offY + y, x - start, 1, line);
        }
    } else {
        for (int x = 0; x < width; x++) line[x] = palette[src[x]];
        spr.pushImage(offX + pDraw->iX, offY + y, width, 1, line);
    }
}

// ─── Кнопки ──────────────────────────────────────────────────────────────────

static bool btnPressed(int pin) {
    if (!digitalRead(pin)) { delay(20); while (!digitalRead(pin)); return true; }
    return false;
}

// ─── Setup / Loop ────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.printf("GIF player: %u assets\n", (unsigned)kGifAssetsCount);

    pinMode(BTN1_PIN, INPUT_PULLUP);
    pinMode(BTN2_PIN, INPUT_PULLUP);

    tft.init();
    tft.setRotation(0);
    tft.setBrightness(220);
    tft.fillScreen((uint16_t)0x0000);

    spr.setColorDepth(16);
    spr.setSwapBytes(true);    // палитра GIF little-endian → свопить при pushImage
    if (!spr.createSprite(240, 240)) {
        spr.setPsram(true);
        if (!spr.createSprite(240, 240)) {
            tft.setTextDatum(lgfx::middle_center);
            tft.setTextColor((uint16_t)0xF800);
            tft.setTextSize(2);
            tft.drawString("NO MEMORY", 120, 120);
            while (true) delay(1000);
        }
    }
    spr.fillSprite((uint16_t)0x0000);

    if (kGifAssetsCount == 0) {
        tft.setTextDatum(lgfx::middle_center);
        tft.setTextColor((uint16_t)0xF800);
        tft.setTextSize(2);
        tft.drawString("NO GIF ASSETS", 120, 120);
        while (true) delay(1000);
    }

    gif.begin(GIF_PALETTE_RGB565_LE);
    if (!openCurrentGif()) {
        Serial.printf("GIF open error: %d\n", gif.getLastError());
        tft.setTextDatum(lgfx::middle_center);
        tft.setTextColor((uint16_t)0xF800);
        tft.setTextSize(2);
        tft.drawString("GIF ERROR", 120, 120);
        while (true) delay(1000);
    }
}

void loop() {
    if (btnPressed(BTN1_PIN)) paused = !paused;
    if (btnPressed(BTN2_PIN)) {
        blOn = !blOn;
        tft.setBrightness(blOn ? 220 : 0);
    }

    if (paused) { delay(10); return; }

    int delayMs = 0;
    if (!gif.playFrame(false, &delayMs)) {
        gif.close();
        gifIndex = (gifIndex + 1) % kGifAssetsCount;   // конец GIF -> следующий
        openCurrentGif();
        return;
    }
    spr.pushSprite(0, 0);

    // Выдержка таймингов кадра (декодирование+вывод уже заняли часть времени)
    static uint32_t lastFrame = 0;
    uint32_t elapsed = millis() - lastFrame;
    if (elapsed < (uint32_t)delayMs) delay(delayMs - elapsed);
    lastFrame = millis();
}
