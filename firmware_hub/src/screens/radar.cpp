#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <math.h>
#include "../core/screen.h"
#include "../core/gfx.h"
#include "../core/net.h"
#include "../core/config.h"
#include "../core/decoders.h"
#include "../secrets.h"

// ─── Радар самолётов (порт firmware_radar) ────────────────────────────────────
// OpenSky Network (анонимно) + опциональная карта-подложка Mapbox (dark style).
// Цвет блипа = высота, размер/форма = категория борта, позиции интерполируются
// между опросами (по icao24). Веб-настройки переехали в общий webui
// (getConfig/applyConfig), координаты центра — глобальные (hubCfg.homeLat/Lon).
//
// BTN1 short — рефреш данных прямо сейчас

#ifndef MAPBOX_TOKEN
#define MAPBOX_TOKEN ""   // пусто = радар без карты, на чёрном фоне
#endif

static const int MAX_PLANES = 60;

namespace radar {

struct Config {
    uint32_t pollSec   = 60;
    float    rangeKm   = 30.0f;
    int      maxPlanes = 60;
    bool     mapDark   = true;
};
static Config cfg;
static Preferences prefs;

static void clampConfig() {
    if (cfg.pollSec < 15)     cfg.pollSec = 15;
    if (cfg.pollSec > 3600)   cfg.pollSec = 3600;
    if (cfg.rangeKm < 10.0f)  cfg.rangeKm = 10.0f;
    if (cfg.rangeKm > 300.0f) cfg.rangeKm = 300.0f;
    if (cfg.maxPlanes < 1)    cfg.maxPlanes = 1;
    if (cfg.maxPlanes > MAX_PLANES) cfg.maxPlanes = MAX_PLANES;
}

static void loadConfig() {
    prefs.begin("radar", true);
    cfg.pollSec   = prefs.getUInt("poll", cfg.pollSec);
    cfg.rangeKm   = prefs.getFloat("range", cfg.rangeKm);
    cfg.maxPlanes = prefs.getInt("maxp", cfg.maxPlanes);
    cfg.mapDark   = prefs.getBool("dark", cfg.mapDark);
    prefs.end();
    clampConfig();
}

static void saveConfig() {
    prefs.begin("radar", false);
    prefs.putUInt("poll", cfg.pollSec);
    prefs.putFloat("range", cfg.rangeKm);
    prefs.putInt("maxp", cfg.maxPlanes);
    prefs.putBool("dark", cfg.mapDark);
    prefs.end();
}

// ─── Карта-подложка (Mapbox static) ──────────────────────────────────────────
// Сжатый PNG живёт в СОБСТВЕННОМ постоянном PSRAM-буфере (не в общем netBuf):
// при возврате на экран карта не перекачивается — только повторный декод в
// mapSprite (~100мс, без сети). Декод — один раз за фетч/заход; drawRadar()
// дергается каждый кадр ради анимации самолётов и просто копирует mapSprite.

static const size_t MAP_BUF_SIZE = 48 * 1024;
static uint8_t* mapBufGet() {
    static uint8_t* buf = nullptr;
    if (!buf) {
        buf = (uint8_t*)ps_malloc(MAP_BUF_SIZE);
        if (!buf) buf = (uint8_t*)malloc(MAP_BUF_SIZE);
    }
    return buf;
}

static size_t  mapLen = 0;
static bool    mapLoaded = false;
static bool    mapSpriteOk = false;   // false = createSprite() провалился
static char    mapErr[24] = "";       // причина неудачи — на экран (серийник капризный)
static bool    mapDirty = true;

static const int RADAR_R = 108;   // радиус радара в пикселях

static LGFX_Sprite mapSprite(&tft);   // закэшированный декодированный фон карты

static int pngDraw(PNGDRAW *pDraw) {
    static uint16_t line[240];
    png.getLineAsRGB565(pDraw, line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
    mapSprite.pushImage(0, pDraw->y, pDraw->iWidth, 1, line);
    return 1;
}

static bool fetchMap() {
    if (!MAPBOX_TOKEN[0]) { strcpy(mapErr, "no token"); return false; }
    uint8_t* mapBuf = mapBufGet();
    if (!mapBuf) { strcpy(mapErr, "no buf"); return false; }

    // Дробный зум подбираем так, чтобы rangeKm пришлось ровно на RADAR_R px:
    // метров/пиксель у веб-меркатора = 156543.03392 * cos(lat) / 2^zoom
    double mpp  = cfg.rangeKm * 1000.0 / RADAR_R;
    double zoom = log(156543.03392 * cos(hubCfg.homeLat * 3.14159265 / 180.0) / mpp) / log(2.0);
    if (zoom < 1.0) zoom = 1.0;
    if (zoom > 19.0) zoom = 19.0;

    const char* style = cfg.mapDark ? "dark-v11" : "light-v11";
    char url[320];
    snprintf(url, sizeof(url),
        "https://api.mapbox.com/styles/v1/mapbox/%s/static/%.5f,%.5f,%.2f/240x240?access_token=%s",
        style, hubCfg.homeLon, hubCfg.homeLat, zoom, MAPBOX_TOKEN);

    statusScreen("Loading map", style, C_CYAN);
    spr.pushSprite(0, 0);   // впереди блокирующий фетч — статус на экран сразу
    // На случай, если предыдущая попытка прервалась посреди чтения тела —
    // secureClient мог остаться в наполовину открытом состоянии. Форсируем
    // чистое соединение.
    secureClient.stop();
    // Без проверки сертификата: цепочка Mapbox не бьётся с Amazon Root CA
    // (X509 verify failed) — для публичных карт приемлемый компромисс.
    secureClient.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    http.begin(secureClient, url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("Mapbox HTTP %d\n", code);
        snprintf(mapErr, sizeof(mapErr), "http %d", code);
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    int remaining = http.getSize();
    mapLen = 0;
    uint32_t lastByteMs = millis();
    const char* exitReason = "maxbuf";
    while (http.connected() && mapLen < MAP_BUF_SIZE) {
        size_t avail = stream->available();
        if (avail > 0) {
            size_t want = min(avail, MAP_BUF_SIZE - mapLen);
            int n = stream->readBytes(mapBuf + mapLen, want);
            mapLen += n;
            if (remaining > 0) { remaining -= n; if (remaining <= 0) { exitReason = "done"; break; } }
            lastByteMs = millis();
        } else if (remaining == 0) {
            exitReason = "remaining0";
            break;
        } else if (millis() - lastByteMs > 5000) {
            exitReason = "stall5s";
            break;
        }
    }
    if (!http.connected() && mapLen == 0) exitReason = "disconnected";
    http.end();
    Serial.printf("Map: %u bytes, zoom %.2f, exit=%s\n", (unsigned)mapLen, zoom, exitReason);
    if (mapLen == 0) {
        snprintf(mapErr, sizeof(mapErr), "0B/%s", exitReason);
        return false;
    }
    mapErr[0] = '\0';
    return true;
}

// Распаковать PNG карты в mapSprite — один раз на каждый fetchMap()/onEnter(),
// не на каждый кадр. Вызывающий код сам гарантирует, что mapBuf/mapLen валидны.
static bool cacheMapBackground() {
    if (!mapSpriteOk) { strcpy(mapErr, "no sprite"); return false; }
    if (png.openRAM(mapBufGet(), mapLen, pngDraw) != PNG_SUCCESS) { strcpy(mapErr, "png open"); return false; }
    int rc = png.decode(nullptr, 0);
    png.close();
    if (rc != PNG_SUCCESS) { strcpy(mapErr, "png decode"); return false; }
    mapErr[0] = '\0';
    return true;
}

// ─── Данные радара ────────────────────────────────────────────────────────────

// icao24 — стабильный идентификатор борта, нужен чтобы сматчить один и тот же
// борт между опросами и интерполировать позицию (callsign бывает пустым).
struct Plane {
    char  icao24[7];
    char  callsign[9];
    float lat, lon;            // T1 — последняя реально полученная позиция
    float prevLat, prevLon;    // T0 — откуда анимируем
    float prevTrack;           // курс на T0 — интерполируем и его
    uint32_t animStartMs;      // millis() начала текущей анимации T0→T1
    uint32_t animDurMs;
    float altM, speedMs, track;
    int8_t category;           // ADS-B emitter category (8=вертолёт, 14=БПЛА...)
};

static Plane planes[MAX_PLANES];
static int   planeCount = 0;

static uint32_t lastFetchMs = 0;
static bool  forceRefresh = true;
static char  planeErr[24] = "";

// Новый борт не тащим через весь интервал опроса — короткий "влёт" выглядит
// естественнее, чем экстраполяция на несколько минут назад.
static const uint32_t NEW_PLANE_ANIM_MS = 15000;
// OpenSky (анонимно) иногда "теряет" борт на один опрос — держим пропавших на
// месте до тайм-аута, иначе борт при возврате дёргается как "новый".
static const uint32_t PLANE_STALE_MS = 3 * 60 * 1000UL;

static const double DEG2RAD    = 3.14159265358979323846 / 180.0;
static const double M_PER_DEG  = 111320.0;

static void latLonToDistBrg(double lat, double lon, float &distM, float &brgDeg) {
    double dx = (lon - hubCfg.homeLon) * M_PER_DEG * cos(hubCfg.homeLat * DEG2RAD);
    double dy = (lat - hubCfg.homeLat) * M_PER_DEG;
    distM  = (float)sqrt(dx * dx + dy * dy);
    brgDeg = (float)(atan2(dx, dy) / DEG2RAD);
    if (brgDeg < 0) brgDeg += 360.0f;
}

// Сдвинуть точку на distM метров по курсу bearingDeg (отрицательный — назад)
static void moveLatLon(double lat, double lon, float bearingDeg, float distM,
                        float &outLat, float &outLon) {
    double rad    = bearingDeg * DEG2RAD;
    double dNorth = distM * cos(rad);
    double dEast  = distM * sin(rad);
    outLat = (float)(lat + dNorth / M_PER_DEG);
    outLon = (float)(lon + dEast / (M_PER_DEG * cos(lat * DEG2RAD)));
}

// Интерполяция угла по кратчайшей стороне (350°→10° идёт через 0°, а не 180°)
static float lerpAngleDeg(float a0, float a1, float t) {
    float diff = fmodf(a1 - a0 + 540.0f, 360.0f) - 180.0f;
    return a0 + diff * t;
}

static bool fetchPlanes() {
    double latDelta = cfg.rangeKm * 1000.0 / M_PER_DEG;
    double lonDelta = cfg.rangeKm * 1000.0 / (M_PER_DEG * cos(hubCfg.homeLat * DEG2RAD));
    char url[224];
    // extended=1 добавляет в конец вектора category (индекс 17) — тип борта
    snprintf(url, sizeof(url),
        "https://opensky-network.org/api/states/all?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f&extended=1",
        hubCfg.homeLat - latDelta, hubCfg.homeLon - lonDelta,
        hubCfg.homeLat + latDelta, hubCfg.homeLon + lonDelta);

    HTTPClient http;
    http.setTimeout(8000);
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("OpenSky HTTP %d (429 = кончились анонимные кредиты на сегодня)\n", code);
        snprintf(planeErr, sizeof(planeErr), "http %d", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    // ArduinoJson-фильтр НЕ умеет выбирать элементы массива по индексам —
    // каждый state-вектор берём целиком, отсекается лишь ключ "time".
    // Индексы: 1=callsign, 5=lon, 6=lat, 7=baro_alt, 8=on_ground, 9=velocity,
    //          10=track, 17=category (при extended=1)
    JsonDocument filter;
    filter["states"][0] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("JSON parse error: %s (payload %u bytes)\n", err.c_str(), payload.length());
        strcpy(planeErr, "json err");
        return false;
    }
    planeErr[0] = '\0';

    JsonArray states = doc["states"].as<JsonArray>();

    // Мержим входящие данные ПРЯМО в живой planes[] (по icao24), а не
    // пересобираем массив с нуля — борт, пропавший на один опрос, остаётся
    // на месте вместо того, чтобы дёргаться экстраполяцией.
    uint32_t now = millis();
    static bool seen[MAX_PLANES];
    memset(seen, 0, sizeof(seen));

    for (JsonArray st : states) {
        if (st[6].isNull() || st[5].isNull()) continue;   // нет позиции
        if (st[8] | false) continue;                       // на земле — не интересно

        const char* icao = st[0] | "";
        char icaoBuf[7];
        strncpy(icaoBuf, icao, sizeof(icaoBuf) - 1);
        icaoBuf[sizeof(icaoBuf) - 1] = '\0';

        int idx = -1;
        for (int j = 0; j < planeCount; j++) {
            if (strcmp(planes[j].icao24, icaoBuf) == 0) { idx = j; break; }
        }
        bool isNewPlane = (idx < 0);
        if (isNewPlane) {
            if (planeCount >= cfg.maxPlanes) continue;
            idx = planeCount++;
        }

        Plane &p = planes[idx];
        strcpy(p.icao24, icaoBuf);
        const char* cs = st[1] | "";
        strncpy(p.callsign, cs, sizeof(p.callsign) - 1);
        p.callsign[sizeof(p.callsign) - 1] = '\0';
        for (int i = strlen(p.callsign) - 1; i >= 0 && p.callsign[i] == ' '; i--) p.callsign[i] = '\0';

        float newLat = st[6], newLon = st[5];
        float newTrack = st[10] | 0.0f;
        p.altM     = st[7] | 0.0f;
        p.speedMs  = st[9] | 0.0f;
        p.category = (int8_t)(st[17] | 0);

        if (isNewPlane) {
            // Короткий "влёт" экстраполяцией назад по курсу/скорости
            float extrapSec = NEW_PLANE_ANIM_MS / 1000.0f;
            moveLatLon(newLat, newLon, newTrack, -p.speedMs * extrapSec, p.prevLat, p.prevLon);
            p.prevTrack = newTrack;
            p.animDurMs = NEW_PLANE_ANIM_MS;
        } else {
            // T0 — последняя реальная позиция, animDurMs — сколько реально
            // прошло (может быть больше pollSec, если борт пропадал).
            p.prevLat   = p.lat;
            p.prevLon   = p.lon;
            p.prevTrack = p.track;
            p.animDurMs = now - p.animStartMs;
        }
        p.lat   = newLat;
        p.lon   = newLon;
        p.track = newTrack;
        p.animStartMs = now;
        seen[idx] = true;
    }

    // Пропавшие борты убираем только по тайм-ауту (см. PLANE_STALE_MS)
    int kept = 0;
    for (int i = 0; i < planeCount; i++) {
        if (seen[i] || (now - planes[i].animStartMs) < PLANE_STALE_MS) {
            if (kept != i) planes[kept] = planes[i];
            kept++;
        }
    }
    planeCount = kept;

    Serial.printf("OpenSky: %d planes in range\n", planeCount);
    return true;
}

// ─── Отрисовка ────────────────────────────────────────────────────────────────

// Цвет по высоте: у земли — оранжевый, эшелон — сине-фиолетовый (как FR24)
static uint16_t altColor(float altM) {
    float t = altM;
    if (t < 0) t = 0;
    if (t > 11000.0f) t = 11000.0f;
    uint8_t hue = 21 + (uint8_t)(t / 11000.0f * 149.0f);
    return hsv565(hue, 255, 255);
}

// Масштаб маркера по категории ADS-B
static float categoryScale(int8_t cat) {
    switch (cat) {
        case 2: case 9: case 12: case 14: return 0.75f;  // лёгкий/планер/ультралайт/БПЛА
        case 3:          return 0.9f;
        case 4: case 5:  return 1.2f;
        case 6:          return 1.45f;   // heavy (B747/A380...)
        default:         return 1.0f;
    }
}

// Иконки бортов: примитивы в спрайт 24x24, на экран через pushRotateZoom
static LGFX_Sprite iconSpr(&spr);
static const uint16_t ICON_TRANSP = 0x0120;   // ключ прозрачности (редкий цвет)

static void makeAirplaneIcon(uint16_t col) {
    iconSpr.fillSprite(ICON_TRANSP);
    iconSpr.fillRoundRect(10, 1, 4, 18, 2, col);          // фюзеляж
    iconSpr.fillTriangle(0, 14, 23, 14, 11, 6, col);      // крылья
    iconSpr.fillTriangle(7, 22, 16, 22, 11, 16, col);     // хвостовое оперение
}

static void makeHeliIcon(uint16_t col) {
    iconSpr.fillSprite(ICON_TRANSP);
    iconSpr.fillEllipse(11, 11, 4, 6, col);
    iconSpr.fillRect(10, 16, 3, 7, col);
    iconSpr.drawLine(3, 3, 20, 19, col);
    iconSpr.drawLine(20, 3, 3, 19, col);
    iconSpr.drawLine(4, 3, 21, 19, col);
    iconSpr.drawLine(21, 3, 4, 19, col);
}

// НЛО для БПЛА: классическая тарелка с огоньками
static void makeUfoIcon(uint16_t col) {
    iconSpr.fillSprite(ICON_TRANSP);
    iconSpr.fillCircle(11, 9, 5, col);
    iconSpr.fillEllipse(11, 14, 11, 4, col);
    iconSpr.fillCircle(4, 14, 1, ICON_TRANSP);
    iconSpr.fillCircle(11, 15, 1, ICON_TRANSP);
    iconSpr.fillCircle(18, 14, 1, ICON_TRANSP);
}

static void drawPlaneIcon(int x, int y, const Plane &p, float headingDeg) {
    if (p.category == 14) {
        makeUfoIcon(hsv565(85, 255, 255));
        iconSpr.pushRotateZoom(&spr, x, y, 0.0f, 1.0f, 1.0f, ICON_TRANSP);
        return;
    }
    uint16_t col  = altColor(p.altM);
    float    size = categoryScale(p.category);
    if (p.category == 8) {
        makeHeliIcon(col);
        iconSpr.pushRotateZoom(&spr, x, y, 0.0f, size, size, ICON_TRANSP);
    } else {
        makeAirplaneIcon(col);
        iconSpr.pushRotateZoom(&spr, x, y, headingDeg, size, size, ICON_TRANSP);
    }
}

// Легенда высоты — градиентная дуга по правому краю (0м внизу → 11км вверху)
static void drawLegend() {
    const float A0 = -60.0f, A1 = 60.0f;
    const float STEP = 4.0f;
    for (float a = A0; a < A1; a += STEP) {
        float t = (a - A0) / (A1 - A0);
        spr.fillArc(120, 120, 119, 113, a, a + STEP + 0.5f, altColor((1.0f - t) * 11000.0f));
    }
    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextSize(1);
    spr.setTextDatum(lgfx::middle_center);
    spr.setTextColor(altColor(11000.0f), C_BLACK);
    spr.drawString("11k", 168, 17);
    spr.setTextColor(altColor(0.0f), C_BLACK);
    spr.drawString("0m", 168, 223);
    spr.setTextDatum(lgfx::middle_right);
    spr.setTextColor(altColor(5500.0f), C_BLACK);
    spr.drawString("6k", 228, 120);
}

static void drawRadar() {
    const int cx = 120, cy = 120;

    if (mapLoaded) mapSprite.pushSprite(&spr, 0, 0);
    else spr.fillSprite(C_BLACK);

    // Сетка поверх карты — приглушённая
    spr.drawCircle(cx, cy, RADAR_R,         C_DGREY);
    spr.drawCircle(cx, cy, RADAR_R * 2 / 3, C_DGREY);
    spr.drawCircle(cx, cy, RADAR_R / 3,     C_DGREY);
    spr.drawLine(cx, cy - RADAR_R, cx, cy + RADAR_R, C_DGREY);
    spr.drawLine(cx - RADAR_R, cy, cx + RADAR_R, cy, C_DGREY);
    spr.fillCircle(cx, cy, 3, C_CYAN);   // дом

    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextDatum(lgfx::middle_center);
    spr.setTextSize(1);
    spr.setTextColor(C_GREY);
    spr.drawString("N", cx, cy - RADAR_R + 8);

    float pxScale = (float)RADAR_R / (cfg.rangeKm * 1000.0f);
    int shown = 0;
    int nearestIdx = -1;
    float nearestDist = 1e12f;
    uint32_t nowMs = millis();

    for (int i = 0; i < planeCount; i++) {
        Plane &p = planes[i];

        // Позиция интерполируется по собственному таймеру борта
        float frac = 1.0f;
        if (p.animDurMs > 0) {
            frac = (nowMs - p.animStartMs) / (float)p.animDurMs;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
        }
        float iLat = p.prevLat + (p.lat - p.prevLat) * frac;
        float iLon = p.prevLon + (p.lon - p.prevLon) * frac;
        float heading = lerpAngleDeg(p.prevTrack, p.track, frac);

        float distM, brgDeg;
        latLonToDistBrg(iLat, iLon, distM, brgDeg);
        if (distM > cfg.rangeKm * 1000.0f) continue;
        float r = distM * pxScale;
        double a = brgDeg * DEG2RAD;
        int x = cx + (int)(r * sin(a));
        int y = cy - (int)(r * cos(a));

        drawPlaneIcon(x, y, p, heading);

        shown++;
        if (distM < nearestDist) { nearestDist = distM; nearestIdx = i; }
    }

    drawLegend();

    // Круглый дисплей: весь текст держим внутри вписанного круга r=120
    char header[24];
    snprintf(header, sizeof(header), "planes: %d", shown);
    spr.setTextDatum(lgfx::top_center);
    spr.setTextColor(C_CYAN, C_BLACK);
    spr.drawString(header, cx, 26);
    snprintf(header, sizeof(header), "range %.0fkm", cfg.rangeKm);
    spr.setTextColor(C_GREY, C_BLACK);
    spr.drawString(header, cx, 38);

    // Ближайший борт: две короткие строки внизу, цвет = высота
    spr.setTextDatum(lgfx::bottom_center);
    if (nearestIdx >= 0) {
        Plane &p = planes[nearestIdx];
        char line[32];
        uint16_t col = altColor(p.altM);
        snprintf(line, sizeof(line), "%s  %.1fkm",
                 p.callsign[0] ? p.callsign : "?", nearestDist / 1000.0f);
        spr.setTextColor(col, C_BLACK);
        spr.drawString(line, cx, 204);
        snprintf(line, sizeof(line), "%.0fm  %.0fkm/h", p.altM, p.speedMs * 3.6f);
        spr.drawString(line, cx, 216);
    } else {
        spr.setTextColor(C_GREY, C_BLACK);
        if (planeErr[0]) {
            char line[40];
            snprintf(line, sizeof(line), "no planes (%s)", planeErr);
            spr.drawString(line, cx, 210);
        } else {
            spr.drawString("no planes", cx, 210);
        }
    }

    // Причина отсутствия карты — мелким текстом у центра
    if (!mapLoaded && mapErr[0]) {
        spr.setTextDatum(lgfx::middle_center);
        spr.setTextColor(C_DGREY, C_BLACK);
        spr.drawString(mapErr, cx, 96);
    }
}

} // namespace radar

class RadarScreen : public Screen {
    bool loaded = false;

public:
    const char* id() const override    { return "radar"; }
    const char* title() const override { return "Flight radar"; }

    void onEnter() override {
        using namespace radar;
        if (!loaded) { loadConfig(); loaded = true; }

        // Спрайт иконки борта: 24x24, поворот/масштаб при выводе
        iconSpr.setColorDepth(16);
        iconSpr.createSprite(24, 24);
        iconSpr.setPivot(11.5f, 11.5f);

        // Кэш декодированного фона карты — 115КБ PSRAM только пока радар активен
        mapSprite.setColorDepth(16);
        mapSprite.setSwapBytes(true);
        mapSprite.setPsram(true);
        mapSpriteOk = mapSprite.createSprite(240, 240);
        if (!mapSpriteOk) {
            mapSprite.setPsram(false);
            mapSpriteOk = mapSprite.createSprite(240, 240);
        }

        // Сжатый PNG карты пережил уход с экрана — если конфиг не менялся,
        // просто декодируем его заново, БЕЗ похода в сеть. Данные бортов тоже
        // не перезапрашиваем принудительно: у OpenSky (аноним) ~400 запросов
        // в день, таймер pollSec сам решит, пора ли.
        mapLoaded = false;
        if (!mapDirty && mapLen > 0) mapLoaded = cacheMapBackground();
        if (!mapLoaded) mapDirty = true;
    }

    void onExit() override {
        using namespace radar;
        mapSprite.deleteSprite();
        iconSpr.deleteSprite();
        mapLoaded = false;
    }

    bool tick(uint32_t nowMs) override {
        using namespace radar;
        if (!netUp()) {
            statusScreen("Radar", "no WiFi", C_RED);
            return true;
        }

        if (mapDirty) {
            mapDirty = false;
            mapLoaded = fetchMap() && cacheMapBackground();
        }

        if (forceRefresh || nowMs - lastFetchMs >= cfg.pollSec * 1000UL) {
            forceRefresh = false;
            lastFetchMs = nowMs;
            fetchPlanes();
        }

        // Рисуем каждый кадр — самолёты интерполируются между T0/T1
        drawRadar();
        return true;
    }

    uint32_t frameDelayMs() const override { return 40; }   // ~25 fps

    void onButton(BtnEvent ev) override {
        if (ev == EV_BTN1_SHORT) radar::forceRefresh = true;
    }

    void getConfig(JsonObject out) override {
        using namespace radar;
        out["pollSec"]   = cfg.pollSec;
        out["rangeKm"]   = cfg.rangeKm;
        out["maxPlanes"] = cfg.maxPlanes;
        out["mapDark"]   = cfg.mapDark;
    }

    void applyConfig(JsonObjectConst in) override {
        using namespace radar;
        float oldRange = cfg.rangeKm;
        bool  oldDark  = cfg.mapDark;
        if (!in["pollSec"].isNull())   cfg.pollSec   = in["pollSec"].as<uint32_t>();
        if (!in["rangeKm"].isNull())   cfg.rangeKm   = in["rangeKm"].as<float>();
        if (!in["maxPlanes"].isNull()) cfg.maxPlanes = in["maxPlanes"].as<int>();
        if (!in["mapDark"].isNull())   cfg.mapDark   = in["mapDark"].as<bool>();
        clampConfig();
        saveConfig();
        if (cfg.rangeKm != oldRange || cfg.mapDark != oldDark) mapDirty = true;
        forceRefresh = true;
    }

    void onConfigChanged() override {   // сменились hubCfg.homeLat/Lon
        radar::mapDirty = true;
        radar::forceRefresh = true;
        radar::planeCount = 0;   // борты старой локации не относятся к новой
    }
};

Screen* radarScreen() {
    static RadarScreen s;
    return &s;
}
