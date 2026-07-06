#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LovyanGFX.hpp>
#include <ArduinoJson.h>
#include <math.h>

// ═══ Стилизованные тучи по реальным данным на GC9A01 240x240 ═════════════════
// Опрашивает Open-Meteo (без ключа) — cloud_cover/precipitation/wind/temperature.
// Кол-во и плотность "туч" на экране следуют cloud_cover, дрейф — направлению и
// скорости ветра, дождь рисуется частицами при precipitation > 0.
//
// BTN1 (GPIO1) — рефреш данных прямо сейчас
// BTN2 (GPIO2) — подсветка вкл/выкл

#include "secrets.h"

static const uint32_t POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;   // 5 минут
static const int      MAX_CLOUDS = 8;
static const int      MAX_RAIN   = 40;

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

// ─── Погода ───────────────────────────────────────────────────────────────────

static float cloudCoverPct = 0, precipMm = 0, tempC = 0, windSpeedKmh = 0, windDirDeg = 0;
static uint32_t lastFetchMs = 0;
static bool forceRefresh = true;

static bool fetchWeather() {
    char url[192];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=cloud_cover,precipitation,temperature_2m,wind_speed_10m,wind_direction_10m",
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
    cloudCoverPct = cur["cloud_cover"]        | 0.0f;
    precipMm      = cur["precipitation"]      | 0.0f;
    tempC         = cur["temperature_2m"]     | 0.0f;
    windSpeedKmh  = cur["wind_speed_10m"]     | 0.0f;
    windDirDeg    = cur["wind_direction_10m"] | 0.0f;
    Serial.printf("cloud=%.0f%% precip=%.1fmm temp=%.1fC wind=%.0fkm/h@%.0f\n",
                  cloudCoverPct, precipMm, tempC, windSpeedKmh, windDirDeg);
    return true;
}

// ─── Тучи и дождь ──────────────────────────────────────────────────────────────

struct Cloud { float x, y, scale, speedMul; };
static Cloud clouds[MAX_CLOUDS];

struct Drop { float x, y; };
static Drop rain[MAX_RAIN];

static const double DEG2RAD = 3.14159265358979323846 / 180.0;

static void initClouds() {
    for (int i = 0; i < MAX_CLOUDS; i++) {
        clouds[i].x = random(0, 240);
        clouds[i].y = random(20, 180);
        clouds[i].scale = 14 + random(0, 14);
        clouds[i].speedMul = 0.6f + (random(0, 100) / 100.0f) * 0.8f;
    }
    for (int i = 0; i < MAX_RAIN; i++) {
        rain[i].x = random(0, 240);
        rain[i].y = random(0, 240);
    }
}

static void drawCloud(float x, float y, float s, uint16_t col) {
    spr.fillCircle((int)(x - s), (int)y, (int)(s * 0.7f), col);
    spr.fillCircle((int)(x + s), (int)y, (int)(s * 0.7f), col);
    spr.fillCircle((int)x, (int)(y - s * 0.5f), (int)(s * 0.9f), col);
    spr.fillCircle((int)x, (int)(y + s * 0.2f), (int)s, col);
}

static void updateAndDraw(float dtSec) {
    // Направление, КУДА дует ветер (метео-направление — откуда дует, +180)
    float toDeg = fmodf(windDirDeg + 180.0f, 360.0f);
    double rad = toDeg * DEG2RAD;
    float speedPxPerSec = constrain(windSpeedKmh * 0.5f, 2.0f, 40.0f);
    float vx = (float)(sin(rad) * speedPxPerSec);
    float vy = (float)(-cos(rad) * speedPxPerSec);

    // Небо темнеет с ростом облачности
    uint8_t skyDark = (uint8_t)(cloudCoverPct * 0.6f);   // 0..60
    uint16_t skyCol = spr.color565(20, 60 - skyDark, 110 - skyDark);
    spr.fillSprite(skyCol);

    int visibleClouds = map((int)cloudCoverPct, 0, 100, 0, MAX_CLOUDS);
    visibleClouds = constrain(visibleClouds, cloudCoverPct > 5 ? 1 : 0, MAX_CLOUDS);
    uint8_t grey = 235 - (uint8_t)(cloudCoverPct * 0.8f);   // пасмурнее — темнее тучи
    uint16_t cloudCol = spr.color565(grey, grey, grey);

    for (int i = 0; i < visibleClouds; i++) {
        Cloud &c = clouds[i];
        c.x += vx * c.speedMul * dtSec;
        c.y += vy * c.speedMul * dtSec;
        if (c.x < -40) c.x += 320; else if (c.x > 280) c.x -= 320;
        if (c.y < -40) c.y += 320; else if (c.y > 280) c.y -= 320;
        drawCloud(c.x, c.y, c.scale, cloudCol);
    }

    int activeRain = constrain((int)(precipMm * 10.0f), 0, MAX_RAIN);
    for (int i = 0; i < activeRain; i++) {
        rain[i].y += 160.0f * dtSec;
        if (rain[i].y > 240) { rain[i].y = -10; rain[i].x = random(0, 240); }
        spr.drawLine((int)rain[i].x, (int)rain[i].y, (int)rain[i].x - 2, (int)rain[i].y + 6,
                     spr.color565(140, 180, 230));
    }

    char buf[16];
    spr.setFont(&lgfx::fonts::Font4);
    spr.setTextDatum(lgfx::middle_center);
    spr.setTextColor(C_WHITE, skyCol);
    snprintf(buf, sizeof(buf), "%.0f°C", tempC);
    spr.drawString(buf, 120, 40);

    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextSize(1);
    spr.setTextColor(C_GREY, skyCol);
    snprintf(buf, sizeof(buf), "cloud %.0f%%", cloudCoverPct);
    spr.setTextDatum(lgfx::bottom_center);
    spr.drawString(buf, 120, 236);

    spr.pushSprite(0, 0);
}

// ─── Кнопки ──────────────────────────────────────────────────────────────────

static bool btnPressed(int pin) {
    if (!digitalRead(pin)) { delay(20); while (!digitalRead(pin)); return true; }
    return false;
}

// ─── Setup / Loop ────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("Stylized clouds widget");

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

    randomSeed(esp_random());
    initClouds();
    connectWiFi();
}

void loop() {
    static uint32_t lastFrameMs = 0;

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
        fetchWeather();
    }

    uint32_t now = millis();
    float dt = (lastFrameMs == 0) ? 0.03f : (now - lastFrameMs) / 1000.0f;
    lastFrameMs = now;
    updateAndDraw(dt);

    delay(30);
}
