#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LovyanGFX.hpp>
#include <ArduinoJson.h>
#include <PNGdec.h>
#include <math.h>

// ═══ Радар самолётов на GC9A01 240x240 ════════════════════════════════════════
// OpenSky Network (анонимно) + опциональная карта-подложка Mapbox (dark style).
// Масштаб карты автоматически подгоняется под RANGE_KM: пиксели радара и
// пиксели карты совпадают, блипы лежат на реальной географии.
//
// Цвет блипа = высота (оранжевый у земли → сине-фиолетовый на эшелоне).
// Размер/форма = категория борта из ADS-B (лёгкий/тяжёлый/вертолёт).
//
// BTN1 (GPIO1) — рефреш данных прямо сейчас
// BTN2 (GPIO2) — подсветка вкл/выкл

#include "secrets.h"

#ifndef MAPBOX_TOKEN
#define MAPBOX_TOKEN ""   // пусто = радар без карты, на чёрном фоне
#endif

// ─── Настройки радара ─────────────────────────────────────────────────────────
// Здесь только дефолты — рабочие значения правятся через веб-морду
// (http://<ip>/ или http://radar.local/) и живут в NVS.

static const int MAX_PLANES = 60;   // размер массива — потолок для cfg.maxPlanes

struct Config {
    uint32_t pollSec   = 60;        // период опроса OpenSky, сек
    float    rangeKm   = 30.0f;     // радиус радара, км
    int      maxPlanes = 60;        // сколько бортов рисуем максимум
    double   lat       = HOME_LAT;  // центр радара
    double   lon       = HOME_LON;
    bool     mapDark   = true;      // тема карты: true=тёмная, false=светлая
};
static Config cfg;
static Preferences prefs;
static bool mapDirty = false;   // радиус/координаты/тема сменились — перекачать карту

static void clampConfig() {
    if (cfg.pollSec < 15)     cfg.pollSec = 15;
    if (cfg.pollSec > 3600)   cfg.pollSec = 3600;
    if (cfg.rangeKm < 10.0f)  cfg.rangeKm = 10.0f;
    if (cfg.rangeKm > 300.0f) cfg.rangeKm = 300.0f;
    if (cfg.maxPlanes < 1)    cfg.maxPlanes = 1;
    if (cfg.maxPlanes > MAX_PLANES) cfg.maxPlanes = MAX_PLANES;
    if (cfg.lat < -85.0)  cfg.lat = -85.0;
    if (cfg.lat > 85.0)   cfg.lat = 85.0;
    if (cfg.lon < -180.0) cfg.lon = -180.0;
    if (cfg.lon > 180.0)  cfg.lon = 180.0;
}

static void loadConfig() {
    prefs.begin("radar", true);
    cfg.pollSec   = prefs.getUInt("poll", cfg.pollSec);
    cfg.rangeKm   = prefs.getFloat("range", cfg.rangeKm);
    cfg.maxPlanes = prefs.getInt("maxp", cfg.maxPlanes);
    cfg.lat       = prefs.getDouble("lat", cfg.lat);
    cfg.lon       = prefs.getDouble("lon", cfg.lon);
    cfg.mapDark   = prefs.getBool("dark", cfg.mapDark);
    prefs.end();
    clampConfig();
}

static void saveConfig() {
    prefs.begin("radar", false);
    prefs.putUInt("poll", cfg.pollSec);
    prefs.putFloat("range", cfg.rangeKm);
    prefs.putInt("maxp", cfg.maxPlanes);
    prefs.putDouble("lat", cfg.lat);
    prefs.putDouble("lon", cfg.lon);
    prefs.putBool("dark", cfg.mapDark);
    prefs.end();
}

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

static const uint16_t C_BLACK = 0x0000;
static const uint16_t C_WHITE = 0xFFFF;
static const uint16_t C_RED   = 0xF800;
static const uint16_t C_CYAN  = 0x07FF;
static const uint16_t C_GREY  = 0x7BEF;
static const uint16_t C_DGREY = 0x39E7;

static bool blOn = true;

static uint16_t hsv565(uint8_t h, uint8_t s, uint8_t v) {
    uint8_t r, g, b;
    if (s == 0) { r = g = b = v; }
    else {
        uint8_t region    = h / 43;
        uint8_t remainder = (h - region * 43) * 6;
        uint8_t p = (v * (255 - s)) >> 8;
        uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
        uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
        switch (region) {
            case 0: r=v; g=t; b=p; break;
            case 1: r=q; g=v; b=p; break;
            case 2: r=p; g=v; b=t; break;
            case 3: r=p; g=q; b=v; break;
            case 4: r=t; g=p; b=v; break;
            default: r=v; g=p; b=q; break;
        }
    }
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

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

// ─── Карта-подложка (Mapbox static, dark) ────────────────────────────────────
// PNG хранится сжатым (mapBuf) и декодируется в спрайт при каждой перерисовке —
// второй полнокадровый буфер не нужен, а перерисовка у нас раз в минуту.

static const size_t MAP_BUF_SIZE = 48 * 1024;
static uint8_t mapBuf[MAP_BUF_SIZE];
static size_t  mapLen = 0;
static bool    mapLoaded = false;

static const int RADAR_R = 108;   // радиус радара в пикселях

static int pngDraw(PNGDRAW *pDraw) {
    static uint16_t line[240];
    png.getLineAsRGB565(pDraw, line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
    spr.pushImage(0, pDraw->y, pDraw->iWidth, 1, line);
    return 1;
}

static bool fetchMap() {
    if (!MAPBOX_TOKEN[0]) return false;   // токена нет — работаем без карты

    // Дробный зум подбираем так, чтобы cfg.rangeKm пришлось ровно на RADAR_R px:
    // метров/пиксель у веб-меркатора = 156543.03392 * cos(lat) / 2^zoom
    double mpp  = cfg.rangeKm * 1000.0 / RADAR_R;
    double zoom = log(156543.03392 * cos(cfg.lat * 3.14159265 / 180.0) / mpp) / log(2.0);
    if (zoom < 1.0) zoom = 1.0;
    if (zoom > 19.0) zoom = 19.0;

    const char* style = cfg.mapDark ? "dark-v11" : "light-v11";
    char url[320];
    snprintf(url, sizeof(url),
        "https://api.mapbox.com/styles/v1/mapbox/%s/static/%.5f,%.5f,%.2f/240x240?access_token=%s",
        style, cfg.lon, cfg.lat, zoom, MAPBOX_TOKEN);

    statusScreen("Loading map", style, C_CYAN);
    // Без проверки сертификата: цепочка Mapbox не бьётся с Amazon Root CA
    // (X509 verify failed), а таскать актуальные корни на девайсе хлопотно.
    // Для публичных карт это приемлемый компромисс.
    secureClient.setInsecure();
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
    Serial.printf("Map: %u bytes, zoom %.2f\n", (unsigned)mapLen, zoom);
    return mapLen > 0;
}

// Распаковать PNG карты в спрайт (фон кадра)
static bool drawMapBackground() {
    if (!mapLoaded) return false;
    if (png.openRAM(mapBuf, mapLen, pngDraw) != PNG_SUCCESS) return false;
    int rc = png.decode(nullptr, 0);
    png.close();
    return rc == PNG_SUCCESS;
}

// ─── Данные радара ────────────────────────────────────────────────────────────

struct Plane {
    char  callsign[9];
    float lat, lon, altM, speedMs, track;
    float distM;    // расстояние от дома, м
    float brgDeg;   // азимут от дома, 0=север, по часовой
    int8_t category; // ADS-B emitter category (0=неизвестно, 8=вертолёт...)
};

static Plane planes[MAX_PLANES];
static int   planeCount = 0;
static uint32_t lastFetchMs = 0;
static bool  forceRefresh = true;

static const double DEG2RAD    = 3.14159265358979323846 / 180.0;
static const double M_PER_DEG  = 111320.0;

static void latLonToDistBrg(double lat, double lon, float &distM, float &brgDeg) {
    double dx = (lon - cfg.lon) * M_PER_DEG * cos(cfg.lat * DEG2RAD); // восток, м
    double dy = (lat - cfg.lat) * M_PER_DEG;                          // север, м
    distM  = (float)sqrt(dx * dx + dy * dy);
    brgDeg = (float)(atan2(dx, dy) / DEG2RAD);
    if (brgDeg < 0) brgDeg += 360.0f;
}

static bool fetchPlanes() {
    double latDelta = cfg.rangeKm * 1000.0 / M_PER_DEG;
    double lonDelta = cfg.rangeKm * 1000.0 / (M_PER_DEG * cos(cfg.lat * DEG2RAD));
    char url[224];
    // extended=1 добавляет в конец вектора category (индекс 17) — тип борта
    snprintf(url, sizeof(url),
        "https://opensky-network.org/api/states/all?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f&extended=1",
        cfg.lat - latDelta, cfg.lon - lonDelta, cfg.lat + latDelta, cfg.lon + lonDelta);

    HTTPClient http;
    http.setTimeout(8000);
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("OpenSky HTTP %d (429 = кончились анонимные кредиты на сегодня)\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    // ArduinoJson-фильтр НЕ умеет выбирать элементы массива по индексам:
    // шаблоном для всех элементов служит только первый элемент фильтра.
    // Поэтому каждый state-вектор берём целиком, отсекается лишь ключ "time".
    // Индексы: 1=callsign, 5=lon, 6=lat, 7=baro_alt, 8=on_ground, 9=velocity,
    //          10=track, 17=category (при extended=1)
    JsonDocument filter;
    filter["states"][0] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("JSON parse error: %s (payload %u bytes)\n", err.c_str(), payload.length());
        return false;
    }

    JsonArray states = doc["states"].as<JsonArray>();
    Serial.printf("HTTP %d, payload %u bytes, states: %u, heap %u\n",
                  code, payload.length(), (unsigned)states.size(), ESP.getFreeHeap());

    planeCount = 0;
    for (JsonArray st : states) {
        if (planeCount >= cfg.maxPlanes) break;
        if (st[6].isNull() || st[5].isNull()) continue;   // нет позиции
        if (st[8] | false) continue;                       // на земле — не интересно

        Plane &p = planes[planeCount];
        const char* cs = st[1] | "";
        strncpy(p.callsign, cs, sizeof(p.callsign) - 1);
        p.callsign[sizeof(p.callsign) - 1] = '\0';
        for (int i = strlen(p.callsign) - 1; i >= 0 && p.callsign[i] == ' '; i--) p.callsign[i] = '\0';

        p.lat      = st[6];
        p.lon      = st[5];
        p.altM     = st[7] | 0.0f;
        p.speedMs  = st[9] | 0.0f;
        p.track    = st[10] | 0.0f;
        p.category = (int8_t)(st[17] | 0);
        latLonToDistBrg(p.lat, p.lon, p.distM, p.brgDeg);
        planeCount++;
    }
    Serial.printf("OpenSky: %d planes in range\n", planeCount);
    return true;
}

// ─── Отрисовка радара ─────────────────────────────────────────────────────────

// Цвет по высоте: у земли — оранжевый, крейсерский эшелон — сине-фиолетовый
// (та же логика, что у FlightRadar24)
static uint16_t altColor(float altM) {
    float t = altM;
    if (t < 0) t = 0;
    if (t > 11000.0f) t = 11000.0f;
    uint8_t hue = 21 + (uint8_t)(t / 11000.0f * 149.0f);   // 21(оранж) → 170(сине-фиол)
    return hsv565(hue, 255, 255);
}

// Масштаб маркера по категории ADS-B:
// 2=Light 3=Small 4=Large 5=HighVortexLarge 6=Heavy 7=HighPerf 8=Rotorcraft
static float categoryScale(int8_t cat) {
    switch (cat) {
        case 2: case 9: case 12: case 14: return 0.75f;  // лёгкий/планер/ультралайт/БПЛА
        case 3:          return 0.9f;
        case 4: case 5:  return 1.2f;
        case 6:          return 1.45f;   // heavy (B747/A380...)
        default:         return 1.0f;
    }
}

// ─── Иконки бортов ────────────────────────────────────────────────────────────
// Рисуются примитивами в спрайт 24x24 (перекрашиваемые в цвет высоты), на экран
// попадают через pushRotateZoom: поворот по курсу + масштаб по категории.
// 24x24 * (0.75..1.45) = 18..35 px на экране.

static LGFX_Sprite iconSpr(&spr);
static const uint16_t ICON_TRANSP = 0x0120;   // ключ прозрачности (редкий цвет)

// Самолёт, нос вверх: фюзеляж + стреловидные крылья + хвост
static void makeAirplaneIcon(uint16_t col) {
    iconSpr.fillSprite(ICON_TRANSP);
    iconSpr.fillRoundRect(10, 1, 4, 18, 2, col);          // фюзеляж
    iconSpr.fillTriangle(0, 14, 23, 14, 11, 6, col);      // крылья
    iconSpr.fillTriangle(7, 22, 16, 22, 11, 16, col);     // хвостовое оперение
}

// Вертолёт: корпус + хвостовая балка + крест лопастей
static void makeHeliIcon(uint16_t col) {
    iconSpr.fillSprite(ICON_TRANSP);
    iconSpr.fillEllipse(11, 11, 4, 6, col);               // корпус
    iconSpr.fillRect(10, 16, 3, 7, col);                  // хвостовая балка
    iconSpr.drawLine(3, 3, 20, 19, col);                  // лопасти
    iconSpr.drawLine(20, 3, 3, 19, col);
    iconSpr.drawLine(4, 3, 21, 19, col);                  // утолщение лопастей
    iconSpr.drawLine(21, 3, 4, 19, col);
}

// НЛО для неопознанных (category 0/1): классическая тарелка с огоньками
static void makeUfoIcon(uint16_t col) {
    iconSpr.fillSprite(ICON_TRANSP);
    iconSpr.fillCircle(11, 9, 5, col);                    // купол
    iconSpr.fillEllipse(11, 14, 11, 4, col);              // тарелка
    iconSpr.fillCircle(4, 14, 1, ICON_TRANSP);            // огоньки (выколоты)
    iconSpr.fillCircle(11, 15, 1, ICON_TRANSP);
    iconSpr.fillCircle(18, 14, 1, ICON_TRANSP);
}

// Нарисовать борт: иконка по категории, цвет по высоте, поворот по курсу.
// Без категории (0/1) — обычный самолёт: лайнеры часто не шлют категорию.
static void drawPlaneIcon(int x, int y, const Plane &p) {
    if (p.category == 14) {                               // БПЛА — НЛО :)
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
        iconSpr.pushRotateZoom(&spr, x, y, p.track, size, size, ICON_TRANSP);
    }
}

// Легенда высоты — градиентная дуга по правому краю экрана (0м внизу → 11км
// вверху), подписи в цвет своей высоты
static void drawLegend() {
    const float A0 = -60.0f, A1 = 60.0f;   // угол: 0 = 3 часа, по часовой
    const float STEP = 4.0f;
    for (float a = A0; a < A1; a += STEP) {
        float t = (a - A0) / (A1 - A0);    // 0 = верхний конец дуги
        spr.fillArc(120, 120, 119, 113, a, a + STEP + 0.5f, altColor((1.0f - t) * 11000.0f));
    }
    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextSize(1);
    spr.setTextDatum(lgfx::middle_center);
    spr.setTextColor(altColor(11000.0f), C_BLACK);
    spr.drawString("11k", 168, 37);        // у верхнего конца дуги
    spr.setTextColor(altColor(0.0f), C_BLACK);
    spr.drawString("0m", 168, 203);        // у нижнего
    spr.setTextDatum(lgfx::middle_right);
    spr.setTextColor(altColor(5500.0f), C_BLACK);
    spr.drawString("6k", 208, 120);        // середина шкалы
}

static void drawRadar() {
    const int cx = 120, cy = 120;

    if (!drawMapBackground()) spr.fillSprite(C_BLACK);

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

    for (int i = 0; i < planeCount; i++) {
        Plane &p = planes[i];
        if (p.distM > cfg.rangeKm * 1000.0f) continue;
        float r = p.distM * pxScale;
        double a = p.brgDeg * DEG2RAD;
        int x = cx + (int)(r * sin(a));
        int y = cy - (int)(r * cos(a));

        drawPlaneIcon(x, y, p);

        shown++;
        if (p.distM < nearestDist) { nearestDist = p.distM; nearestIdx = i; }
    }

    drawLegend();

    // Круглый дисплей: весь текст держим внутри вписанного круга r=120.
    // Шапка — две строки по центру сверху
    char header[24];
    snprintf(header, sizeof(header), "planes: %d", shown);
    spr.setTextDatum(lgfx::top_center);
    spr.setTextColor(C_CYAN, C_BLACK);
    spr.drawString(header, cx, 26);
    snprintf(header, sizeof(header), "range %.0fkm", cfg.rangeKm);
    spr.setTextColor(C_GREY, C_BLACK);
    spr.drawString(header, cx, 38);

    // Ближайший борт: две короткие строки внизу по центру, цвет = высота
    spr.setTextDatum(lgfx::bottom_center);
    if (nearestIdx >= 0) {
        Plane &p = planes[nearestIdx];
        char line[32];
        uint16_t col = altColor(p.altM);
        snprintf(line, sizeof(line), "%s  %.1fkm",
                 p.callsign[0] ? p.callsign : "?", p.distM / 1000.0f);
        spr.setTextColor(col, C_BLACK);
        spr.drawString(line, cx, 204);
        snprintf(line, sizeof(line), "%.0fm  %.0fkm/h", p.altM, p.speedMs * 3.6f);
        spr.drawString(line, cx, 216);
    } else {
        spr.setTextColor(C_GREY, C_BLACK);
        spr.drawString("no planes", cx, 210);
    }

    spr.pushSprite(0, 0);
}

// ─── Веб-сервер настроек (http://<ip>/ или http://radar.local/) ───────────────

static WebServer server(80);

struct City { const char* name; float lat, lon; };
static const City CITIES[] = {
    {"Istanbul", 41.01f, 28.98f},   {"Ankara", 39.93f, 32.86f},    {"Moscow", 55.76f, 37.62f},
    {"London", 51.51f, -0.13f},     {"Paris", 48.86f, 2.35f},      {"Berlin", 52.52f, 13.40f},
    {"Madrid", 40.42f, -3.70f},     {"Rome", 41.90f, 12.50f},      {"Amsterdam", 52.37f, 4.90f},
    {"Brussels", 50.85f, 4.35f},    {"Vienna", 48.21f, 16.37f},    {"Warsaw", 52.23f, 21.01f},
    {"Prague", 50.08f, 14.44f},     {"Kyiv", 50.45f, 30.52f},      {"Minsk", 53.90f, 27.57f},
    {"Athens", 37.98f, 23.73f},     {"Lisbon", 38.72f, -9.14f},    {"Stockholm", 59.33f, 18.06f},
    {"Oslo", 59.91f, 10.75f},       {"Copenhagen", 55.68f, 12.57f},{"Helsinki", 60.17f, 24.94f},
    {"Dublin", 53.35f, -6.26f},     {"Bern", 46.95f, 7.45f},       {"Budapest", 47.50f, 19.04f},
    {"Bucharest", 44.43f, 26.10f},  {"Belgrade", 44.79f, 20.45f},  {"Sofia", 42.70f, 23.32f},
    {"Tbilisi", 41.72f, 44.79f},    {"Yerevan", 40.18f, 44.51f},   {"Baku", 40.41f, 49.87f},
    {"Astana", 51.13f, 71.43f},     {"Tashkent", 41.30f, 69.24f},  {"Bishkek", 42.87f, 74.59f},
    {"Dubai", 25.20f, 55.27f},      {"Doha", 25.29f, 51.53f},      {"Riyadh", 24.71f, 46.68f},
    {"Cairo", 30.04f, 31.24f},      {"Tokyo", 35.68f, 139.69f},    {"Beijing", 39.90f, 116.40f},
    {"Seoul", 37.57f, 126.98f},     {"Delhi", 28.61f, 77.21f},     {"Bangkok", 13.76f, 100.50f},
    {"Singapore", 1.35f, 103.82f},  {"Jakarta", -6.21f, 106.85f},  {"Hanoi", 21.03f, 105.85f},
    {"Washington", 38.90f, -77.04f},{"Ottawa", 45.42f, -75.70f},   {"Mexico City", 19.43f, -99.13f},
    {"Brasilia", -15.79f, -47.88f}, {"Buenos Aires", -34.60f, -58.38f}, {"Canberra", -35.28f, 149.13f},
};
static const int CITIES_COUNT = sizeof(CITIES) / sizeof(CITIES[0]);

static void handleRoot() {
    String h;
    h.reserve(10240);
    h += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>MiniScreen Radar</title><style>"
        "body{font-family:sans-serif;background:#111;color:#eee;max-width:420px;margin:20px auto;padding:0 12px}"
        "label{display:block;margin:12px 0 4px;color:#8cf}"
        "input,select{width:100%;padding:8px;background:#222;color:#eee;border:1px solid #444;border-radius:6px;box-sizing:border-box}"
        "button{margin-top:16px;width:100%;padding:10px;background:#17a05e;border:0;border-radius:6px;color:#fff;font-size:16px}"
        ".row{display:flex;gap:8px}.row>div{flex:1}h1{font-size:20px}p{color:#777;font-size:13px}"
        "</style></head><body><h1>&#9992; Радар — настройки</h1><form action='/save'>");

    h += F("<label>Период обновления, сек (15–3600)</label>"
           "<input name='p' type='number' min='15' max='3600' value='");
    h += String(cfg.pollSec);
    h += F("'><label>Радиус, км (10–300)</label>"
           "<input name='r' type='number' min='10' max='300' value='");
    h += String((int)cfg.rangeKm);
    h += F("'><label>Макс. самолётов (1–60)</label>"
           "<input name='m' type='number' min='1' max='60' value='");
    h += String(cfg.maxPlanes);

    h += F("'><label>Тема карты</label><select name='t'>");
    h += cfg.mapDark ? F("<option value='1' selected>тёмная</option><option value='0'>светлая</option>")
                     : F("<option value='1'>тёмная</option><option value='0' selected>светлая</option>");
    h += F("</select>");

    h += F("<label>Город (подставит координаты)</label><select onchange=\""
           "var o=this.selectedOptions[0];if(o.dataset.la){"
           "document.getElementById('la').value=o.dataset.la;"
           "document.getElementById('lo').value=o.dataset.lo}\">"
           "<option>— выбрать —</option>");
    for (int i = 0; i < CITIES_COUNT; i++) {
        // город, совпадающий с текущими координатами, показываем выбранным
        bool cur = fabs(cfg.lat - CITIES[i].lat) < 0.02 && fabs(cfg.lon - CITIES[i].lon) < 0.02;
        h += F("<option data-la='");
        h += String(CITIES[i].lat, 4);
        h += F("' data-lo='");
        h += String(CITIES[i].lon, 4);
        h += cur ? F("' selected>") : F("'>");
        h += CITIES[i].name;
        h += F("</option>");
    }
    h += F("</select><div class='row'><div><label>Широта</label>"
           "<input id='la' name='lat' value='");
    h += String(cfg.lat, 6);
    h += F("'></div><div><label>Долгота</label><input id='lo' name='lon' value='");
    h += String(cfg.lon, 6);
    h += F("'></div></div><button>Сохранить</button></form><p>Бортов сейчас: ");
    h += planeCount;
    h += mapLoaded ? F(" | карта OK") : F(" | без карты");
    h += F(" | heap ");
    h += ESP.getFreeHeap();
    h += F("<br>OpenSky аноним: ~400 запросов/день — не ставь период слишком малым</p>"
           "</body></html>");
    server.send(200, "text/html", h);
}

static void handleSave() {
    double oldLat = cfg.lat, oldLon = cfg.lon;
    float  oldRange = cfg.rangeKm;
    bool   oldDark  = cfg.mapDark;

    if (server.hasArg("p"))   cfg.pollSec   = server.arg("p").toInt();
    if (server.hasArg("r"))   cfg.rangeKm   = server.arg("r").toFloat();
    if (server.hasArg("m"))   cfg.maxPlanes = server.arg("m").toInt();
    if (server.hasArg("t"))   cfg.mapDark   = server.arg("t").toInt() != 0;
    if (server.hasArg("lat")) cfg.lat       = server.arg("lat").toDouble();
    if (server.hasArg("lon")) cfg.lon       = server.arg("lon").toDouble();
    clampConfig();
    saveConfig();

    if (cfg.lat != oldLat || cfg.lon != oldLon || cfg.rangeKm != oldRange || cfg.mapDark != oldDark)
        mapDirty = true;   // карту перекачает loop, чтобы не блокировать HTTP-ответ
    forceRefresh = true;

    server.sendHeader("Location", "/");
    server.send(303);
    Serial.printf("Config saved: poll=%us range=%.0fkm max=%d lat=%.4f lon=%.4f %s\n",
                  cfg.pollSec, cfg.rangeKm, cfg.maxPlanes, cfg.lat, cfg.lon,
                  cfg.mapDark ? "dark" : "light");
}

static void startWebServer() {
    MDNS.begin("radar");   // http://radar.local
    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.begin();
    Serial.printf("Web UI: http://%s/ (http://radar.local/)\n", WiFi.localIP().toString().c_str());
}

// ─── Кнопки ──────────────────────────────────────────────────────────────────

static bool btnPressed(int pin) {
    if (!digitalRead(pin)) { delay(20); while (!digitalRead(pin)); return true; }
    return false;
}

// ─── Setup / Loop ────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("Plane radar v4-web");
    Serial.printf("PSRAM: %u bytes, heap: %u\n", ESP.getPsramSize(), ESP.getFreeHeap());

    pinMode(BTN1_PIN, INPUT_PULLUP);
    pinMode(BTN2_PIN, INPUT_PULLUP);

    tft.init();
    tft.setRotation(0);
    tft.setBrightness(220);
    tft.fillScreen(C_BLACK);

    spr.setColorDepth(16);
    spr.setSwapBytes(true);   // строки PNG приходят little-endian (тот же урок, что с GIF)
    spr.setPsram(true);       // фреймбуфер в PSRAM: освобождает 115КБ heap для TLS
    if (!spr.createSprite(240, 240)) {
        spr.setPsram(false);
        if (!spr.createSprite(240, 240)) {
            tft.setTextDatum(lgfx::middle_center);
            tft.setTextColor(C_RED);
            tft.setTextSize(2);
            tft.drawString("NO MEMORY", 120, 120);
            while (true) delay(1000);
        }
    }

    // Спрайт иконки борта: 24x24, поворот/масштаб при выводе
    iconSpr.setColorDepth(16);
    iconSpr.createSprite(24, 24);
    iconSpr.setPivot(11.5f, 11.5f);

    loadConfig();

    if (connectWiFi()) {
        String ip = WiFi.localIP().toString();
        statusScreen("Web UI", ip.c_str(), C_CYAN);   // адрес настроек — на экран
        delay(2000);
        startWebServer();
        mapLoaded = fetchMap();
        if (!mapLoaded) Serial.println("Радар без карты (нет токена или ошибка загрузки)");
    }
}

void loop() {
    server.handleClient();

    if (btnPressed(BTN1_PIN)) forceRefresh = true;
    if (btnPressed(BTN2_PIN)) {
        blOn = !blOn;
        tft.setBrightness(blOn ? 220 : 0);
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (!connectWiFi()) { delay(3000); return; }
    }

    if (mapDirty) {           // настройки сменились — перекачать карту
        mapDirty = false;
        mapLoaded = fetchMap();
    }

    if (forceRefresh || millis() - lastFetchMs >= cfg.pollSec * 1000UL) {
        forceRefresh = false;
        lastFetchMs = millis();
        if (fetchPlanes()) drawRadar();
    }

    delay(10);
}
