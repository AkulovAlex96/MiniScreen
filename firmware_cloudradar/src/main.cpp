#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LovyanGFX.hpp>
#include <ArduinoJson.h>
#include <PNGdec.h>
#include <math.h>

// ═══ Настоящий радар/спутник облаков (RainViewer) на GC9A01 240x240 ══════════
// Тянет метаданные с api.rainviewer.com (без ключа), берёт самый свежий кадр
// радара, вычисляет тайл вокруг HOME_LAT/HOME_LON и декодирует PNG (PNGdec).
// Условия RainViewer требуют атрибуцию — она нарисована внизу экрана.
//
// BTN1 (GPIO1) — рефреш тайла прямо сейчас
// BTN2 (GPIO2) — подсветка вкл/выкл

#include "secrets.h"

static const uint32_t POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;   // радар обновляется раз в 10 мин
static const int      ZOOM       = 6;      // 0-7, глобальная сетка RainViewer
static const int      TILE_SIZE  = 256;
static const int      COLOR_SCHEME = 2;    // 0-8, см. rainviewer.com/api.html
static const char*    TILE_OPTIONS  = "1_1"; // smooth_snow

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
static PNG png;

#define BTN1_PIN 1
#define BTN2_PIN 2

static const uint16_t C_WHITE = 0xFFFF;
static const uint16_t C_RED   = 0xF800;
static const uint16_t C_CYAN  = 0x07FF;
static const uint16_t C_GREY  = 0x7BEF;

// "Небо" — фон, на который блендятся прозрачные пиксели тайла
static const uint8_t SKY_R = 8, SKY_G = 16, SKY_B = 36;

static bool blOn = true;

// ─── Wi-Fi: 2 сети, приоритет первой ──────────────────────────────────────────

struct WifiNet { const char* ssid; const char* pass; };
static const WifiNet WIFI_NETS[] = {
    { WIFI_SSID_1, WIFI_PASS_1 },   // приоритетная
    { WIFI_SSID_2, WIFI_PASS_2 },   // резервная
};
static const int WIFI_NETS_COUNT = sizeof(WIFI_NETS) / sizeof(WIFI_NETS[0]);

static void statusScreen(const char* line1, const char* line2, uint16_t col) {
    spr.fillSprite(0x0000);
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

static bool connectWiFi() {
    WiFi.mode(WIFI_STA);
    for (int i = 0; i < WIFI_NETS_COUNT; i++) {
        if (!WIFI_NETS[i].ssid || !WIFI_NETS[i].ssid[0]) continue;
        statusScreen("WiFi...", WIFI_NETS[i].ssid, C_CYAN);
        WiFi.begin(WIFI_NETS[i].ssid, WIFI_NETS[i].pass);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) delay(200);
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("WiFi OK: %s (%s)\n", WIFI_NETS[i].ssid, WiFi.localIP().toString().c_str());
            return true;
        }
        WiFi.disconnect(true);
        delay(200);
    }
    statusScreen("WiFi FAIL", "no networks reachable", C_RED);
    return false;
}

// ─── Тайл RainViewer ──────────────────────────────────────────────────────────

static const size_t PNG_BUF_SIZE = 48 * 1024;   // тайл 256x256 обычно 2-20 КБ
static uint8_t pngBuf[PNG_BUF_SIZE];
static size_t  pngLen = 0;
static uint32_t lastFetchMs = 0;
static bool forceRefresh = true;

static bool fetchToBuffer(const String &url, uint8_t *buf, size_t bufSize, size_t &outLen) {
    HTTPClient http;
    http.setTimeout(8000);
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("GET %s -> HTTP %d\n", url.c_str(), code);
        http.end();
        return false;
    }
    WiFiClient *stream = http.getStreamPtr();
    int remaining = http.getSize();   // -1, если chunked/неизвестно
    outLen = 0;
    uint32_t lastByteMs = millis();
    while (http.connected() && outLen < bufSize) {
        size_t avail = stream->available();
        if (avail > 0) {
            size_t want = min(avail, bufSize - outLen);
            int n = stream->readBytes(buf + outLen, want);
            outLen += n;
            if (remaining > 0) { remaining -= n; if (remaining <= 0) break; }
            lastByteMs = millis();
        } else if (remaining == 0) {
            break;
        } else if (millis() - lastByteMs > 5000) {
            Serial.println("fetchToBuffer: timeout");
            break;
        }
    }
    http.end();
    return outLen > 0;
}

// Стандартная slippy-map формула (Web Mercator)
static void lonLatToTile(double lon, double lat, int zoom, int &x, int &y) {
    double n = (double)(1 << zoom);
    x = (int)((lon + 180.0) / 360.0 * n);
    double latRad = lat * M_PI / 180.0;
    y = (int)((1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * n);
}

static int tileOffX = 0, tileOffY = 0;

static int pngDraw(PNGDRAW *pDraw) {
    static uint16_t line[TILE_SIZE];
    uint32_t bkgd = SKY_R | (SKY_G << 8) | (SKY_B << 16);   // формат PNGdec: R|G<<8|B<<16
    png.getLineAsRGB565(pDraw, line, PNG_RGB565_LITTLE_ENDIAN, bkgd);
    spr.pushImage(tileOffX, tileOffY + pDraw->y, pDraw->iWidth, 1, line);
    return 1;
}

static bool fetchAndDrawTile() {
    HTTPClient http;
    http.setTimeout(8000);
    http.begin("https://api.rainviewer.com/public/weather-maps.json");
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("RainViewer meta HTTP %d\n", code);
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    JsonArray past = doc["radar"]["past"].as<JsonArray>();
    if (past.isNull() || past.size() == 0) {
        Serial.println("RainViewer: no radar frames");
        return false;
    }
    String host = doc["host"].as<String>();
    String path = past[past.size() - 1]["path"].as<String>();

    int tx, ty;
    lonLatToTile(HOME_LON, HOME_LAT, ZOOM, tx, ty);

    String url = host + path + "/" + String(TILE_SIZE) + "/" + String(ZOOM) + "/" +
                 String(tx) + "/" + String(ty) + "/" + String(COLOR_SCHEME) + "/" + TILE_OPTIONS + ".png";
    Serial.printf("Tile: %s\n", url.c_str());

    if (!fetchToBuffer(url, pngBuf, PNG_BUF_SIZE, pngLen)) return false;

    int rc = png.openRAM(pngBuf, pngLen, pngDraw);
    if (rc != PNG_SUCCESS) {
        Serial.printf("PNG open error: %d\n", png.getLastError());
        return false;
    }

    tileOffX = (240 - png.getWidth())  / 2;
    tileOffY = (240 - png.getHeight()) / 2;

    spr.setSwapBytes(true);   // getLineAsRGB565 отдаёт little-endian (как GIF/JPEG в проекте)
    spr.fillSprite(spr.color565(SKY_R, SKY_G, SKY_B));
    rc = png.decode(nullptr, 0);
    png.close();
    if (rc != PNG_SUCCESS) {
        Serial.printf("PNG decode error: %d\n", png.getLastError());
        return false;
    }

    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextDatum(lgfx::bottom_center);
    spr.setTextSize(1);
    spr.setTextColor(C_GREY, spr.color565(SKY_R, SKY_G, SKY_B));
    spr.drawString("Weather data by RainViewer", 120, 236);   // атрибуция обязательна по их ToS

    spr.pushSprite(0, 0);
    return true;
}

// ─── Кнопки ──────────────────────────────────────────────────────────────────

static bool btnPressed(int pin) {
    if (!digitalRead(pin)) { delay(20); while (!digitalRead(pin)); return true; }
    return false;
}

// ─── Setup / Loop ────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("Cloud radar (RainViewer)");

    pinMode(BTN1_PIN, INPUT_PULLUP);
    pinMode(BTN2_PIN, INPUT_PULLUP);

    tft.init();
    tft.setRotation(0);
    tft.setBrightness(220);
    tft.fillScreen(0x0000);

    spr.setColorDepth(16);
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
}

void loop() {
    if (btnPressed(BTN1_PIN)) forceRefresh = true;
    if (btnPressed(BTN2_PIN)) {
        blOn = !blOn;
        tft.setBrightness(blOn ? 220 : 0);
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (!connectWiFi()) { delay(3000); return; }
    }

    if (forceRefresh || millis() - lastFetchMs >= POLL_INTERVAL_MS) {
        forceRefresh = false;
        lastFetchMs = millis();
        if (!fetchAndDrawTile()) statusScreen("No data", "retry next cycle", C_RED);
    }

    delay(200);
}
