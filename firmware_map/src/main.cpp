#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LovyanGFX.hpp>
#include <JPEGDEC.h>
#include <ArduinoJson.h>

// ═══ Карта города + погода текстом на GC9A01 240x240 ═════════════════════════
// Карта — Mapbox Static Images API (нужен бесплатный access token), фетчится
// редко (при старте и по BTN1). Погода — Open-Meteo (без ключа), обновляется
// по таймеру и рисуется поверх карты плашкой с текстом, без перезапроса карты.
//
// BTN1 (GPIO1) — рефреш карты и погоды прямо сейчас
// BTN2 (GPIO2) — подсветка вкл/выкл

#include "secrets.h"

static const int      MAP_ZOOM             = 14;    // масштаб карты (город/квартал)
static const uint32_t WEATHER_INTERVAL_MS  = 5UL * 60UL * 1000UL;   // 5 минут

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

static const uint16_t C_WHITE = 0xFFFF;
static const uint16_t C_RED   = 0xF800;
static const uint16_t C_CYAN  = 0x07FF;
static const uint16_t C_GREY  = 0x7BEF;

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

// ─── Карта (Mapbox Static Images) ─────────────────────────────────────────────

static const size_t MAP_BUF_SIZE = 48 * 1024;   // JPEG 240x240 обычно 8-30 КБ
static uint8_t mapBuf[MAP_BUF_SIZE];
static size_t  mapLen = 0;
static bool    mapLoaded = false;

static int jpgDraw(JPEGDRAW *p) {
    spr.pushImage(p->x, p->y, p->iWidth, p->iHeight, p->pPixels);
    return 1;
}

static bool fetchMap() {
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.mapbox.com/styles/v1/mapbox/streets-v12/static/%.5f,%.5f,%d/240x240.jpg90?access_token=%s",
        (double)HOME_LON, (double)HOME_LAT, MAP_ZOOM, MAPBOX_TOKEN);

    HTTPClient http;
    http.setTimeout(10000);
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("Mapbox HTTP %d\n", code);
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    int remaining = http.getSize();
    mapLen = 0;
    uint32_t lastByteMs = millis();
    while (http.connected() && mapLen < MAP_BUF_SIZE) {
        size_t avail = stream->available();
        if (avail > 0) {
            size_t want = min(avail, MAP_BUF_SIZE - mapLen);
            int n = stream->readBytes(mapBuf + mapLen, want);
            mapLen += n;
            if (remaining > 0) { remaining -= n; if (remaining <= 0) break; }
            lastByteMs = millis();
        } else if (remaining == 0) {
            break;
        } else if (millis() - lastByteMs > 5000) {
            Serial.println("fetchMap: timeout");
            break;
        }
    }
    http.end();
    if (mapLen == 0) return false;

    if (!jpeg.openRAM(mapBuf, mapLen, jpgDraw)) {
        Serial.printf("JPEG open error: %d\n", jpeg.getLastError());
        return false;
    }
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    spr.setSwapBytes(true);
    bool ok = jpeg.decode(0, 0, 0);
    jpeg.close();
    if (!ok) Serial.println("JPEG decode error");
    return ok;
}

// ─── Погода (Open-Meteo) ──────────────────────────────────────────────────────

static float tempC = 0, cloudCoverPct = 0;
static int   weatherCode = 0;
static uint32_t lastWeatherMs = 0;

static const char* weatherLabel(int code) {
    if (code == 0) return "clear";
    if (code <= 3) return "cloudy";
    if (code == 45 || code == 48) return "fog";
    if (code <= 67) return "rain";
    if (code <= 86) return "snow";
    return "storm";
}

static bool fetchWeather() {
    char url[192];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,cloud_cover,weather_code",
        (double)HOME_LAT, (double)HOME_LON);

    HTTPClient http;
    http.setTimeout(8000);
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("Open-Meteo HTTP %d\n", code);
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

    JsonObject cur = doc["current"];
    tempC         = cur["temperature_2m"] | 0.0f;
    cloudCoverPct = cur["cloud_cover"]    | 0.0f;
    weatherCode   = cur["weather_code"]   | 0;
    Serial.printf("temp=%.1fC cloud=%.0f%% code=%d\n", tempC, cloudCoverPct, weatherCode);
    return true;
}

static void drawWeatherBanner() {
    spr.fillRoundRect(20, 184, 200, 52, 10, spr.color565(10, 10, 10));
    spr.drawRoundRect(20, 184, 200, 52, 10, C_GREY);

    char line1[16], line2[24];
    snprintf(line1, sizeof(line1), "%.0fC", tempC);
    snprintf(line2, sizeof(line2), "%s  cloud %.0f%%", weatherLabel(weatherCode), cloudCoverPct);

    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextDatum(lgfx::middle_center);
    spr.setTextSize(2);
    spr.setTextColor(C_WHITE);
    spr.drawString(line1, 120, 200);
    spr.setTextSize(1);
    spr.setTextColor(C_GREY);
    spr.drawString(line2, 120, 220);
}

// ─── Кнопки ──────────────────────────────────────────────────────────────────

static bool btnPressed(int pin) {
    if (!digitalRead(pin)) { delay(20); while (!digitalRead(pin)); return true; }
    return false;
}

// ─── Setup / Loop ────────────────────────────────────────────────────────────

static bool forceRefresh = true;

void setup() {
    Serial.begin(115200);
    Serial.println("City map + weather");

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

    bool redraw = false;

    if (!mapLoaded || forceRefresh) {
        statusScreen("Map...", "loading", C_CYAN);
        mapLoaded = fetchMap();
        redraw = true;
    }

    if (forceRefresh || millis() - lastWeatherMs >= WEATHER_INTERVAL_MS) {
        lastWeatherMs = millis();
        if (fetchWeather()) redraw = true;
    }

    forceRefresh = false;

    if (redraw && mapLoaded) {
        drawWeatherBanner();
        spr.pushSprite(0, 0);
    } else if (!mapLoaded) {
        statusScreen("Map error", "check MAPBOX_TOKEN", C_RED);
    }

    delay(200);
}
