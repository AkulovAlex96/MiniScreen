#include <HTTPClient.h>
#include "../core/screen.h"
#include "../core/gfx.h"
#include "../core/net.h"
#include "../core/config.h"
#include "../core/decoders.h"
#include "../secrets.h"

// ─── Карта города + погода текстом (порт firmware_map) ────────────────────────
// Карта — Mapbox Static Images (нужен токен), фетчится редко. Погода —
// Open-Meteo, обновляется по таймеру и рисуется плашкой поверх карты.
// Оба хоста — с настоящей проверкой сертификата (см. core/net.h: ROOT_CA_*).
//
// BTN1 short — рефреш карты и погоды прямо сейчас

#ifndef MAPBOX_TOKEN
#define MAPBOX_TOKEN ""
#endif

namespace citymap {

static const int      MAP_ZOOM            = 14;   // масштаб (город/квартал)
static const uint32_t WEATHER_INTERVAL_MS = 5UL * 60UL * 1000UL;

static bool mapLoaded = false;
static uint32_t lastWeatherMs = 0;
static bool forceRefresh = true;

static int pngDraw(PNGDRAW *pDraw) {
    static uint16_t line[240];
    png.getLineAsRGB565(pDraw, line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);   // карта непрозрачная
    spr.pushImage(0, pDraw->y, pDraw->iWidth, 1, line);
    return 1;
}

static bool fetchMap() {
    if (!MAPBOX_TOKEN[0]) return false;
    char url[256];
    // Без суффикса формата: для векторных стилей Mapbox отдаёт PNG,
    // явный .jpg90 на streets-v12 даёт 404 (проверено curl).
    snprintf(url, sizeof(url),
        "https://api.mapbox.com/styles/v1/mapbox/streets-v12/static/%.5f,%.5f,%d/240x240?access_token=%s",
        hubCfg.homeLon, hubCfg.homeLat, MAP_ZOOM, MAPBOX_TOKEN);

    secureClient.stop();
    secureClient.setCACert(ROOT_CA_AMAZON);
    uint8_t* buf = netBufGet();
    size_t len = 0;
    if (!buf || !fetchToBuffer(url, buf, NET_BUF_SIZE, len, &secureClient)) return false;

    int rc = png.openRAM(buf, len, pngDraw);
    if (rc != PNG_SUCCESS) {
        Serial.printf("PNG open error: %d\n", png.getLastError());
        return false;
    }
    rc = png.decode(nullptr, 0);
    png.close();
    if (rc != PNG_SUCCESS) Serial.printf("PNG decode error: %d\n", png.getLastError());
    return rc == PNG_SUCCESS;
}

static float tempC = 0, cloudCoverPct = 0;
static int   weatherCode = 0;

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
        hubCfg.homeLat, hubCfg.homeLon);

    secureClient.stop();
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

} // namespace citymap

class CityMapScreen : public Screen {
public:
    const char* id() const override    { return "city_map"; }
    const char* title() const override { return "City map"; }

    void onEnter() override {
        // spr перезаписан другим экраном — карту надо перекачать/перерисовать
        citymap::mapLoaded = false;
        citymap::forceRefresh = true;
    }

    void tick(uint32_t nowMs) override {
        using namespace citymap;
        if (!netUp()) {
            statusScreen("City map", "no WiFi", C_RED);
            return;
        }

        bool redraw = false;

        if (!mapLoaded || forceRefresh) {
            statusScreen("Map...", "loading", C_CYAN);
            mapLoaded = fetchMap();
            redraw = true;
        }

        if (forceRefresh || nowMs - lastWeatherMs >= WEATHER_INTERVAL_MS) {
            lastWeatherMs = nowMs;
            if (fetchWeather()) redraw = true;
        }

        forceRefresh = false;

        if (redraw && mapLoaded) {
            drawWeatherBanner();
            spr.pushSprite(0, 0);
        } else if (!mapLoaded) {
            statusScreen("Map error", "check MAPBOX_TOKEN", C_RED);
        }
    }

    uint32_t frameDelayMs() const override { return 200; }

    void onButton(BtnEvent ev) override {
        if (ev == EV_BTN1_SHORT) citymap::forceRefresh = true;
    }

    void onConfigChanged() override {
        citymap::mapLoaded = false;
        citymap::forceRefresh = true;
    }
};

Screen* cityMapScreen() {
    static CityMapScreen s;
    return &s;
}
