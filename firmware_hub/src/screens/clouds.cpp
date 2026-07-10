#include <HTTPClient.h>
#include <math.h>
#include "../core/screen.h"
#include "../core/gfx.h"
#include "../core/net.h"
#include "../core/config.h"

// ─── Стилизованные тучи по реальным данным (порт firmware_clouds) ─────────────
// Open-Meteo (без ключа): cloud_cover/precipitation/wind/temperature.
// Кол-во и плотность туч следуют cloud_cover, дрейф — ветру, дождь — частицами.
//
// BTN1 short — рефреш данных прямо сейчас

namespace clouds {

static const uint32_t POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;
static const int MAX_CLOUDS = 8;
static const int MAX_RAIN   = 40;

static float cloudCoverPct = 0, precipMm = 0, tempC = 0, windSpeedKmh = 0, windDirDeg = 0;
static uint32_t lastFetchMs = 0;
static bool forceRefresh = true;

static bool fetchWeather() {
    char url[192];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=cloud_cover,precipitation,temperature_2m,wind_speed_10m,wind_direction_10m",
        hubCfg.homeLat, hubCfg.homeLon);

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

struct Cloud { float x, y, scale, speedMul; };
static Cloud cloudsArr[MAX_CLOUDS];

struct Drop { float x, y; };
static Drop rain[MAX_RAIN];

static const double DEG2RAD = 3.14159265358979323846 / 180.0;

static void initClouds() {
    for (int i = 0; i < MAX_CLOUDS; i++) {
        cloudsArr[i].x = random(0, 240);
        cloudsArr[i].y = random(20, 180);
        cloudsArr[i].scale = 14 + random(0, 14);
        cloudsArr[i].speedMul = 0.6f + (random(0, 100) / 100.0f) * 0.8f;
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
    // Направление, КУДА дует ветер (метео-направление — откуда, +180)
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
        Cloud &c = cloudsArr[i];
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
}

} // namespace clouds

class CloudsScreen : public Screen {
    bool inited = false;
    uint32_t lastFrameMs = 0;

public:
    const char* id() const override    { return "clouds"; }
    const char* title() const override { return "Clouds"; }

    void onEnter() override {
        if (!inited) { clouds::initClouds(); inited = true; }
        lastFrameMs = 0;
    }

    bool tick(uint32_t nowMs) override {
        using namespace clouds;
        if (!netUp()) {
            statusScreen("Clouds", "no WiFi", C_RED);
            return true;
        }

        if (forceRefresh || nowMs - lastFetchMs >= POLL_INTERVAL_MS) {
            forceRefresh = false;
            lastFetchMs = nowMs;
            fetchWeather();
        }

        float dt = (lastFrameMs == 0) ? 0.03f : (nowMs - lastFrameMs) / 1000.0f;
        lastFrameMs = nowMs;
        updateAndDraw(dt);
        return true;
    }

    uint32_t frameDelayMs() const override { return 30; }

    void onButton(BtnEvent ev) override {
        if (ev == EV_BTN1_SHORT) clouds::forceRefresh = true;
    }

    void onConfigChanged() override { clouds::forceRefresh = true; }
};

Screen* cloudsScreen() {
    static CloudsScreen s;
    return &s;
}
