#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LovyanGFX.hpp>
#include <ArduinoJson.h>
#include <math.h>

// ═══ Радар самолётов на GC9A01 240x240 ════════════════════════════════════════
// Опрашивает OpenSky Network (анонимно, без ключа) по бокс-запросу вокруг
// HOME_LAT/HOME_LON и рисует борта как блипы на круговом радаре.
//
// BTN1 (GPIO1) — рефреш данных прямо сейчас
// BTN2 (GPIO2) — подсветка вкл/выкл

#include "secrets.h"

// ─── Настройки радара (не секрет, крутится в коде) ───────────────────────────

static const double   RANGE_KM         = 500.0;     // радиус отображения
static const uint32_t POLL_INTERVAL_MS = 20;    // 60000; OpenSky: анонимно 400 "кредитов"/день
static const int      MAX_PLANES       = 60;

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

static const uint16_t C_BLACK = 0x0000;
static const uint16_t C_WHITE = 0xFFFF;
static const uint16_t C_RED   = 0xF800;
static const uint16_t C_CYAN  = 0x07FF;
static const uint16_t C_GREY  = 0x7BEF;
static const uint16_t C_GREEN = 0x07E0;

static bool blOn = true;

// ─── Wi-Fi: 2 сети, приоритет первой ──────────────────────────────────────────

struct WifiNet { const char* ssid; const char* pass; };
static const WifiNet WIFI_NETS[] = {
    { WIFI_SSID_1, WIFI_PASS_1 },   // приоритетная
    { WIFI_SSID_2, WIFI_PASS_2 },   // резервная
};
static const int WIFI_NETS_COUNT = sizeof(WIFI_NETS) / sizeof(WIFI_NETS[0]);

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

// Перебирает сети по порядку (снова с приоритетной каждый вызов), чтобы при
// возврате более приоритетной сети устройство само на неё переключилось.
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

// ─── Данные радара ────────────────────────────────────────────────────────────

struct Plane {
    char  callsign[9];
    float lat, lon, altM, speedMs, track;
    float distM;   // расстояние от дома, м
    float brgDeg;  // азимут от дома, 0=север, по часовой
};

static Plane planes[MAX_PLANES];
static int   planeCount = 0;
static uint32_t lastFetchMs = 0;
static bool  forceRefresh = true;

static const double DEG2RAD    = 3.14159265358979323846 / 180.0;
static const double M_PER_DEG  = 111320.0;

static void latLonToDistBrg(double lat, double lon, float &distM, float &brgDeg) {
    double dx = (lon - HOME_LON) * M_PER_DEG * cos(HOME_LAT * DEG2RAD); // восток, м
    double dy = (lat - HOME_LAT) * M_PER_DEG;                          // север, м
    distM  = (float)sqrt(dx * dx + dy * dy);
    brgDeg = (float)(atan2(dx, dy) / DEG2RAD);
    if (brgDeg < 0) brgDeg += 360.0f;
}

static bool fetchPlanes() {
    double latDelta = RANGE_KM * 1000.0 / M_PER_DEG;
    double lonDelta = RANGE_KM * 1000.0 / (M_PER_DEG * cos(HOME_LAT * DEG2RAD));
    char url[192];
    snprintf(url, sizeof(url),
        "https://opensky-network.org/api/states/all?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
        HOME_LAT - latDelta, HOME_LON - lonDelta, HOME_LAT + latDelta, HOME_LON + lonDelta);

    HTTPClient http;
    http.setTimeout(8000);
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("OpenSky HTTP %d\n", code);
        http.end();
        return false;
    }

    // Фильтр: держим только нужные индексы в каждом state-векторе —
    // 1=callsign, 5=lon, 6=lat, 7=baro_altitude, 8=on_ground, 9=velocity, 10=true_track
    JsonDocument filter;
    JsonArray fState = filter["states"].add<JsonArray>();
    for (int i = 0; i <= 10; i++) {
        fState.add(i == 1 || i == 5 || i == 6 || i == 7 || i == 8 || i == 9 || i == 10);
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    planeCount = 0;
    for (JsonArray st : doc["states"].as<JsonArray>()) {
        if (planeCount >= MAX_PLANES) break;
        if (st[6].isNull() || st[5].isNull()) continue;   // нет позиции
        if (st[8] | false) continue;                       // на земле — не интересно

        Plane &p = planes[planeCount];
        const char* cs = st[1] | "";
        strncpy(p.callsign, cs, sizeof(p.callsign) - 1);
        p.callsign[sizeof(p.callsign) - 1] = '\0';
        // OpenSky отдаёт callsign с паддингом пробелами — обрежем
        for (int i = strlen(p.callsign) - 1; i >= 0 && p.callsign[i] == ' '; i--) p.callsign[i] = '\0';

        p.lat     = st[6];
        p.lon     = st[5];
        p.altM    = st[7] | 0.0f;
        p.speedMs = st[9] | 0.0f;
        p.track   = st[10] | 0.0f;
        latLonToDistBrg(p.lat, p.lon, p.distM, p.brgDeg);
        planeCount++;
    }
    Serial.printf("OpenSky: %d planes in range\n", planeCount);
    return true;
}

// ─── Отрисовка радара ─────────────────────────────────────────────────────────

static void drawPlaneMarker(int x, int y, float trackDeg, uint16_t col) {
    double a = trackDeg * DEG2RAD;
    int tipX  = x + (int)(7 * sin(a));
    int tipY  = y - (int)(7 * cos(a));
    spr.fillCircle(x, y, 3, col);
    spr.drawLine(x, y, tipX, tipY, col);
}

static void drawRadar() {
    spr.fillSprite(C_BLACK);

    const int cx = 120, cy = 120, rMax = 108;
    spr.drawCircle(cx, cy, rMax,     C_GREY);
    spr.drawCircle(cx, cy, rMax * 2 / 3, C_GREY);
    spr.drawCircle(cx, cy, rMax / 3, C_GREY);
    spr.drawLine(cx, cy - rMax, cx, cy + rMax, C_GREY);
    spr.drawLine(cx - rMax, cy, cx + rMax, cy, C_GREY);
    spr.fillCircle(cx, cy, 2, C_CYAN);

    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextDatum(lgfx::middle_center);
    spr.setTextSize(1);
    spr.setTextColor(C_GREY);
    spr.drawString("N", cx, cy - rMax + 8);

    float scale = (float)rMax / (RANGE_KM * 1000.0f);
    int shown = 0;
    int nearestIdx = -1;
    float nearestDist = 1e12f;

    for (int i = 0; i < planeCount; i++) {
        Plane &p = planes[i];
        if (p.distM > RANGE_KM * 1000.0f) continue;
        float r = p.distM * scale;
        double a = p.brgDeg * DEG2RAD;
        int x = cx + (int)(r * sin(a));
        int y = cy - (int)(r * cos(a));
        drawPlaneMarker(x, y, p.track, C_GREEN);
        shown++;
        if (p.distM < nearestDist) { nearestDist = p.distM; nearestIdx = i; }
    }

    char header[24];
    snprintf(header, sizeof(header), "planes: %d", shown);
    spr.setTextDatum(lgfx::top_left);
    spr.setTextColor(C_CYAN, C_BLACK);
    spr.drawString(header, 4, 4);

    spr.setTextDatum(lgfx::bottom_center);
    if (nearestIdx >= 0) {
        Plane &p = planes[nearestIdx];
        char line[40];
        snprintf(line, sizeof(line), "%s  %.0fm  %.0fkm/h",
                 p.callsign[0] ? p.callsign : "?", p.altM, p.speedMs * 3.6f);
        spr.setTextColor(C_WHITE, C_BLACK);
        spr.drawString(line, cx, 236);
    } else {
        spr.setTextColor(C_GREY, C_BLACK);
        spr.drawString("no planes in range", cx, 236);
    }

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
    Serial.println("Plane radar");

    pinMode(BTN1_PIN, INPUT_PULLUP);
    pinMode(BTN2_PIN, INPUT_PULLUP);

    tft.init();
    tft.setRotation(0);
    tft.setBrightness(220);
    tft.fillScreen(C_BLACK);

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
        if (fetchPlanes()) drawRadar();
    }

    delay(50);
}
