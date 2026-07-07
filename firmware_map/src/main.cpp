#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LovyanGFX.hpp>
#include <PNGdec.h>
#include <ArduinoJson.h>

// http.begin(url) с неявным insecure-фолбэком для https:// на этой связке
// плата+framework надёжно ловит start_ssl_client: -1 (проверено — не зависит
// от того, вызывать ли setInsecure() самому и в каком порядке). Обходим,
// проверяя настоящую цепочку сертификатов — корневые CA ниже подтверждены
// напрямую (openssl s_client/curl -v на api.mapbox.com и api.open-meteo.com).

static const char* ROOT_CA_AMAZON = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----
)EOF";

static const char* ROOT_CA_ISRG = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

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
static PNG png;
static WiFiClientSecure secureClient;

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

static const size_t MAP_BUF_SIZE = 48 * 1024;   // PNG 240x240 у Mapbox обычно 1-20 КБ (векторный стиль)
static uint8_t mapBuf[MAP_BUF_SIZE];
static size_t  mapLen = 0;
static bool    mapLoaded = false;

static int pngDraw(PNGDRAW *pDraw) {
    static uint16_t line[240];
    png.getLineAsRGB565(pDraw, line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);   // без блендинга — карта непрозрачная
    spr.pushImage(0, pDraw->y, pDraw->iWidth, 1, line);
    return 1;
}

static bool fetchMap() {
    char url[256];
    // Без суффикса формата (.jpg90 и т.п.) — Mapbox сам решает: для векторных
    // стилей типа streets-v12 отдаёт PNG. Явный .jpg90 на этот стиль даёт 404
    // (проверено напрямую curl-запросом, не догадка).
    snprintf(url, sizeof(url),
        "https://api.mapbox.com/styles/v1/mapbox/streets-v12/static/%.5f,%.5f,%d/240x240?access_token=%s",
        (double)HOME_LON, (double)HOME_LAT, MAP_ZOOM, MAPBOX_TOKEN);

    secureClient.setCACert(ROOT_CA_AMAZON);
    HTTPClient http;
    http.setTimeout(10000);
    http.begin(secureClient, url);
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

    int rc = png.openRAM(mapBuf, mapLen, pngDraw);
    if (rc != PNG_SUCCESS) {
        Serial.printf("PNG open error: %d\n", png.getLastError());
        return false;
    }
    spr.setSwapBytes(true);
    rc = png.decode(nullptr, 0);
    png.close();
    if (rc != PNG_SUCCESS) Serial.printf("PNG decode error: %d\n", png.getLastError());
    return rc == PNG_SUCCESS;
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

    secureClient.setCACert(ROOT_CA_ISRG);
    HTTPClient http;
    http.setTimeout(8000);
    http.begin(secureClient, url);
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
