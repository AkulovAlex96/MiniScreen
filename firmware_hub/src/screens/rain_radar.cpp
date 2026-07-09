#include <HTTPClient.h>
#include <math.h>
#include "../core/screen.h"
#include "../core/gfx.h"
#include "../core/net.h"
#include "../core/config.h"
#include "../core/decoders.h"

// ─── Настоящий радар осадков (порт firmware_cloudradar, RainViewer) ───────────
// Метаданные с api.rainviewer.com (без ключа), самый свежий кадр радара,
// тайл вокруг дома декодируется PNGdec. Условия RainViewer требуют атрибуцию —
// она нарисована внизу экрана.
//
// BTN1 short — рефреш тайла прямо сейчас

namespace rainradar {

static const uint32_t POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;
static const int      ZOOM         = 6;      // 0-7, глобальная сетка RainViewer
static const int      TILE_SIZE    = 256;
static const int      COLOR_SCHEME = 2;
static const char*    TILE_OPTIONS = "1_1";  // smooth_snow

// "Небо" — фон, на который блендятся прозрачные пиксели тайла
static const uint8_t SKY_R = 8, SKY_G = 16, SKY_B = 36;

static uint32_t lastFetchMs = 0;
static bool forceRefresh = true;

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
    lonLatToTile(hubCfg.homeLon, hubCfg.homeLat, ZOOM, tx, ty);

    String url = host + path + "/" + String(TILE_SIZE) + "/" + String(ZOOM) + "/" +
                 String(tx) + "/" + String(ty) + "/" + String(COLOR_SCHEME) + "/" + TILE_OPTIONS + ".png";
    Serial.printf("Tile: %s\n", url.c_str());

    uint8_t* buf = netBufGet();
    size_t len = 0;
    if (!buf || !fetchToBuffer(url, buf, NET_BUF_SIZE, len)) return false;

    int rc = png.openRAM(buf, len, pngDraw);
    if (rc != PNG_SUCCESS) {
        Serial.printf("PNG open error: %d\n", png.getLastError());
        return false;
    }

    tileOffX = (240 - png.getWidth())  / 2;
    tileOffY = (240 - png.getHeight()) / 2;

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
    spr.drawString("Weather data by RainViewer", 120, 236);   // атрибуция обязательна по ToS

    spr.pushSprite(0, 0);
    return true;
}

} // namespace rainradar

class RainRadarScreen : public Screen {
public:
    const char* id() const override    { return "rain_radar"; }
    const char* title() const override { return "Rain radar"; }

    void onEnter() override {
        // spr перезаписан другим экраном — перерисовать тайл (и заодно обновить)
        rainradar::forceRefresh = true;
    }

    void tick(uint32_t nowMs) override {
        using namespace rainradar;
        if (!netUp()) {
            statusScreen("Rain radar", "no WiFi", C_RED);
            return;
        }

        if (forceRefresh || nowMs - lastFetchMs >= POLL_INTERVAL_MS) {
            forceRefresh = false;
            lastFetchMs = nowMs;
            if (!fetchAndDrawTile()) statusScreen("No data", "retry next cycle", C_RED);
        }
        // Между фетчами кадр статичен — ничего не пушим
    }

    uint32_t frameDelayMs() const override { return 200; }

    void onButton(BtnEvent ev) override {
        if (ev == EV_BTN1_SHORT) rainradar::forceRefresh = true;
    }

    void onConfigChanged() override { rainradar::forceRefresh = true; }
};

Screen* rainRadarScreen() {
    static RainRadarScreen s;
    return &s;
}
