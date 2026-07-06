#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LovyanGFX.hpp>
#include <JPEGDEC.h>

// ═══ MJPEG-стриминг из сети на GC9A01 240x240 ═════════════════════════════════
// Сервер (ffmpeg) отдаёт MJPEG по HTTP → ESP32 вылавливает JPEG-кадры из потока
// (по маркерам SOI FFD8 / EOI FFD9), декодирует JPEGDEC'ом в спрайт и выводит.
//
// Сервер: см. server/stream.bat (ffmpeg -listen 1 -f mpjpeg ...)
//
// BTN1 (GPIO1) — вкл/выкл счётчик FPS
// BTN2 (GPIO2) — подсветка

// ─── Настройки Wi-Fi и адрес потока ──────────────────────────────────────────
// Лежат в src/secrets.h (не в git). Нет файла → скопируй secrets.h.example

#include "secrets.h"

// ─── LGFX: конфиг панели (тот же, что в других прошивках) ────────────────────

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
static JPEGDEC jpeg;

#define BTN1_PIN 1
#define BTN2_PIN 2

static const uint16_t C_BLACK = 0x0000;
static const uint16_t C_WHITE = 0xFFFF;
static const uint16_t C_RED   = 0xF800;
static const uint16_t C_CYAN  = 0x07FF;
static const uint16_t C_GREY  = 0x7BEF;

// ─── Буфер кадра и парсер потока ─────────────────────────────────────────────

static const size_t FBUF_SIZE = 48 * 1024;   // JPEG 240x240 обычно 8-25 КБ
static uint8_t fbuf[FBUF_SIZE];
static size_t  flen = 0;

static uint8_t chunk[2048];
enum ParseState { HUNT_SOI, IN_FRAME };
static ParseState pstate = HUNT_SOI;
static uint8_t prevByte = 0;

static HTTPClient http;
static WiFiClient* stream = nullptr;
static uint32_t lastDataMs = 0;

static bool showFps = true;
static bool blOn    = true;
static int  fps = 0, frameCnt = 0, dropCnt = 0;
static uint32_t fpsWindowMs = 0;

// ─── Статусный экран ─────────────────────────────────────────────────────────

static void statusScreen(const char* line1, const char* line2, uint16_t col) {
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
    spr.pushSprite(0, 0);
}

// ─── JPEG → спрайт ───────────────────────────────────────────────────────────

static int offX = 0, offY = 0;

static int jpgDraw(JPEGDRAW* p) {
    spr.pushImage(p->x + offX, p->y + offY, p->iWidth, p->iHeight, p->pPixels);
    return 1;
}

static void decodeAndShow() {
    if (!jpeg.openRAM(fbuf, flen, jpgDraw)) { dropCnt++; return; }
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);   // + setSwapBytes(true) на спрайте

    offX = (240 - jpeg.getWidth())  / 2;
    offY = (240 - jpeg.getHeight()) / 2;

    if (jpeg.decode(0, 0, 0)) {
        frameCnt++;
        if (showFps) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%d fps", fps);
            spr.setFont(&lgfx::fonts::Font0);
            spr.setTextDatum(lgfx::middle_center);
            spr.setTextSize(1);
            spr.setTextColor(C_CYAN, C_BLACK);
            spr.drawString(buf, 120, 222);
        }
        spr.pushSprite(0, 0);
    } else {
        dropCnt++;
    }
    jpeg.close();

    uint32_t nowMs = millis();
    if (nowMs - fpsWindowMs >= 1000) {
        fps = frameCnt;
        if (dropCnt) Serial.printf("fps=%d dropped=%d\n", fps, dropCnt);
        frameCnt = 0; dropCnt = 0;
        fpsWindowMs = nowMs;
    }
}

// ─── Wi-Fi и HTTP ────────────────────────────────────────────────────────────

static void connectWiFi() {
    statusScreen("WiFi...", WIFI_SSID, C_CYAN);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) delay(200);
    Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
}

static bool openStream() {
    statusScreen("Connecting", STREAM_URL, C_CYAN);
    http.end();
    http.setReuse(false);
    http.setTimeout(5000);
    http.begin(STREAM_URL);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char buf[24];
        snprintf(buf, sizeof(buf), "HTTP %d", code);
        statusScreen("No stream", buf, C_RED);
        Serial.printf("Stream open failed: %d\n", code);
        http.end();
        return false;
    }
    stream = http.getStreamPtr();
    pstate = HUNT_SOI;
    flen = 0;
    prevByte = 0;
    lastDataMs = millis();
    Serial.println("Stream opened");
    return true;
}

// Разбор куска потока: ловим FFD8 ... FFD9, целый кадр → декодировать
static void feedParser(const uint8_t* data, int n) {
    for (int i = 0; i < n; i++) {
        uint8_t b = data[i];
        if (pstate == HUNT_SOI) {
            if (prevByte == 0xFF && b == 0xD8) {
                fbuf[0] = 0xFF; fbuf[1] = 0xD8;
                flen = 2;
                pstate = IN_FRAME;
            }
        } else {
            if (flen >= FBUF_SIZE) {          // кадр не влез — сброс
                pstate = HUNT_SOI;
                flen = 0;
            } else {
                fbuf[flen++] = b;
                if (prevByte == 0xFF && b == 0xD9) {
                    decodeAndShow();
                    pstate = HUNT_SOI;
                    flen = 0;
                }
            }
        }
        prevByte = b;
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
    Serial.println("MJPEG stream player");

    pinMode(BTN1_PIN, INPUT_PULLUP);
    pinMode(BTN2_PIN, INPUT_PULLUP);

    tft.init();
    tft.setRotation(0);
    tft.setBrightness(220);
    tft.fillScreen(C_BLACK);

    spr.setColorDepth(16);
    spr.setSwapBytes(true);    // данные приходят little-endian (как с GIF)
    if (!spr.createSprite(240, 240)) {
        spr.setPsram(true);
        if (!spr.createSprite(240, 240)) {
            tft.setTextDatum(lgfx::middle_center);
            tft.setTextColor(C_RED);
            tft.setTextSize(2);
            tft.drawString("NO MEMORY", 120, 120);
            while (true) delay(1000);
        }
    }

    connectWiFi();
    fpsWindowMs = millis();
}

void loop() {
    if (btnPressed(BTN1_PIN)) showFps = !showFps;
    if (btnPressed(BTN2_PIN)) {
        blOn = !blOn;
        tft.setBrightness(blOn ? 220 : 0);
    }

    if (WiFi.status() != WL_CONNECTED) {
        stream = nullptr;
        connectWiFi();
        return;
    }

    if (!stream || !http.connected()) {
        stream = nullptr;
        if (!openStream()) delay(2000);
        return;
    }

    int avail = stream->available();
    if (avail > 0) {
        int n = stream->readBytes(chunk, min(avail, (int)sizeof(chunk)));
        if (n > 0) {
            feedParser(chunk, n);
            lastDataMs = millis();
        }
    } else if (millis() - lastDataMs > 5000) {
        Serial.println("Stream stalled, reconnecting");
        stream = nullptr;   // 5 сек тишины → переподключение
    }
}
