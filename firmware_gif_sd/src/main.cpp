#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <sys/time.h>
#include <LovyanGFX.hpp>
#include <AnimatedGIF.h>
#include <vector>

// ═══ Проигрывание GIF с MicroSD на GC9A01 240x240 ═════════════════════════════
// В отличие от firmware_gif (гифки зашиты во flash как C-массивы), тут гифки
// читаются файлами прямо с карты — никакого convert_gif.py/пересборки, просто
// скинуть .gif на карту в папку /gif (или в корень). Файлами можно управлять
// и по Wi-Fi — простая HTTP-страница со списком/загрузкой/удалением.
//
// MicroSD — отдельная SPI-шина (HSPI/SPI3), не общая с дисплеем: та же
// разводка, что в firmware_sdcard.
//   GND->GND  VCC->5V(!)  MISO->GPIO41  MOSI->GPIO40  SCK->GPIO39  CS->GPIO38
// VCC именно 5V — на плате свой AMS1117-3.3 с просадкой ~1В, от 3.3V на
// выходе будет ~2.0-2.3V, SD-карте не хватает для стабильной инициализации.
// Линии CS/SCK/MOSI/MISO сидят на регулированных 3.3V, безопасны для ESP32.
//
// BTN1 (GPIO1) — короткое нажатие: следующий GIF. Долгое (>600мс): предыдущий GIF
// BTN2 (GPIO2) — подсветка вкл/выкл
// Сенсорная кнопка (GPIO15) — переключить экран: GIF -> сеть -> календарь -> GIF

#include "secrets.h"

#define SD_CS   38
#define SD_SCK  39
#define SD_MOSI 40
#define SD_MISO 41

#define TOUCH_PIN 15

static const char*    GIF_DIR           = "/gif";   // сначала ищем тут, потом в корне
static const uint32_t SD_RETRY_MS       = 1500;
static const uint32_t BTN_LONG_PRESS_MS = 600;
static const uint32_t WIFI_RETRY_MS     = 30000;   // если не смогли подключиться — пробовать заново раз в 30с
static const long     TZ_OFFSET_SEC     = 7L * 3600;   // UTC+7 — поправьте под свой часовой пояс

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
static AnimatedGIF gif;
static SPIClass sdSPI(HSPI);   // независимая от дисплея (SPI2) шина
static WebServer server(80);

#define BTN1_PIN 1
#define BTN2_PIN 2

static const uint16_t C_BLACK  = 0x0000;
static const uint16_t C_WHITE  = 0xFFFF;
static const uint16_t C_RED    = 0xF800;
static const uint16_t C_CYAN   = 0x07FF;
static const uint16_t C_GREY   = 0x7BEF;
static const uint16_t C_ACCENT = 0x051F;

static const uint8_t BRIGHTNESS_LEVELS[] = { 220, 130, 60, 0 };
static const int     BRIGHTNESS_LEVELS_COUNT = sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);
static int      brightnessIdx  = 0;
static bool sdMounted   = false;
static uint32_t lastSdAttemptMs = 0;

enum ScreenMode { MODE_GIF, MODE_NETWORK, MODE_CALENDAR };
static ScreenMode screenMode   = MODE_GIF;
static uint32_t   lastModeDrawMs = 0;

// ─── Wi-Fi: 2 сети, приоритет первой ──────────────────────────────────────────

struct WifiNet { const char* ssid; const char* pass; };
static const WifiNet WIFI_NETS[] = {
    { WIFI_SSID_1, WIFI_PASS_1 },   // приоритетная
    { WIFI_SSID_2, WIFI_PASS_2 },   // резервная
};
static const int WIFI_NETS_COUNT = sizeof(WIFI_NETS) / sizeof(WIFI_NETS[0]);
static bool     wifiUp = false;
static uint32_t lastWifiAttemptMs = 0;

// Файловый менеджер по Wi-Fi не обязателен для проигрывания — плеер работает
// с картой и без сети, поэтому подключение здесь не блокирует запуск (в
// отличие от firmware_radar/clouds/cloudradar/map, где сеть — основная функция).
static bool connectWiFiOnce() {
    WiFi.mode(WIFI_STA);
    for (int i = 0; i < WIFI_NETS_COUNT; i++) {
        if (!WIFI_NETS[i].ssid || !WIFI_NETS[i].ssid[0]) continue;
        WiFi.begin(WIFI_NETS[i].ssid, WIFI_NETS[i].pass);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) delay(200);
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("WiFi OK: %s (%s)\n", WIFI_NETS[i].ssid, WiFi.localIP().toString().c_str());
            return true;
        }
        WiFi.disconnect(true);
    }
    Serial.println("WiFi: no networks reachable, will retry later");
    return false;
}

// ─── Время (NTP) и календарь ──────────────────────────────────────────────────
// Реальное время по сети — раз есть Wi-Fi, не нужен компилятор-как-часы хак
// из firmware_widgets. Без синка (нет сети/ещё не пришло) — календарь честно
// показывает "не синхронизировано", а не 1970 год.

static bool timeSynced = false;

static void syncTimeIfNeeded() {
    if (timeSynced || !wifiUp) return;
    configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
    struct tm tmInfo;
    if (getLocalTime(&tmInfo, 2000)) {
        timeSynced = true;
        Serial.printf("Время синхронизировано: %04d-%02d-%02d %02d:%02d\n",
                      tmInfo.tm_year + 1900, tmInfo.tm_mon + 1, tmInfo.tm_mday,
                      tmInfo.tm_hour, tmInfo.tm_min);
    }
}

struct DateTime { int year, mon, day, h, m, s, wday; };   // wday: 0=Mon..6=Sun

static const char* MONTHS[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};

static bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int daysInMonth(int y, int m) {
    static const int d[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return (m == 2 && isLeap(y)) ? 29 : d[m - 1];
}

// Через mktime/localtime (а не Zeller), чтобы не тащить отдельную реализацию —
// стандартная либа уже корректно знает про високосные года и переносы.
static int firstWeekdayOfMonth(int year, int mon) {
    struct tm tmv = {};
    tmv.tm_year = year - 1900;
    tmv.tm_mon  = mon - 1;
    tmv.tm_mday = 1;
    tmv.tm_hour = 12;   // подальше от полуночи, чтобы не зацепить сдвиг DST
    time_t t = mktime(&tmv);
    struct tm out;
    localtime_r(&t, &out);
    return (out.tm_wday + 6) % 7;   // tm_wday: 0=Sun..6=Sat -> 0=Mon..6=Sun
}

static DateTime nowDT() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    DateTime dt;
    dt.year = tmv.tm_year + 1900;
    dt.mon  = tmv.tm_mon + 1;
    dt.day  = tmv.tm_mday;
    dt.h    = tmv.tm_hour;
    dt.m    = tmv.tm_min;
    dt.s    = tmv.tm_sec;
    dt.wday = (tmv.tm_wday + 6) % 7;
    return dt;
}

struct GifEntry { String path; String name; };
static std::vector<GifEntry> gifFiles;
static size_t gifIndex = 0;

static int offX = 0, offY = 0;

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

// ─── Экран сети (по клику сенсорной кнопки) ──────────────────────────────────

static void drawNetworkScreen() {
    spr.fillSprite(C_BLACK);
    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextDatum(lgfx::middle_center);
    spr.setTextColor(C_ACCENT);
    spr.setTextSize(2);
    spr.drawString("Network", 120, 70);
    spr.setTextSize(1);
    if (wifiUp && WiFi.status() == WL_CONNECTED) {
        spr.setTextColor(C_WHITE);
        spr.drawString(WiFi.SSID(), 120, 115);
        spr.setTextColor(C_GREY);
        spr.drawString(WiFi.localIP().toString(), 120, 140);
    } else {
        spr.setTextColor(C_RED);
        spr.drawString("not connected", 120, 120);
    }
    spr.pushSprite(0, 0);
}

// ─── Экран календаря (по клику сенсорной кнопки) ─────────────────────────────
// Порт wCalendar() из firmware_widgets, время — из nowDT() (NTP), а не из
// компилятора-как-часов (там сети не было).

// Кольцо насечек по контуру (как у аналоговых часов) + 3 толстые метки —
// текущие час/минута/секунда поверх обычных насечек. Секунда (и чуть-чуть
// минута) — с дробной частью через gettimeofday(), иначе стрелка дискретно
// прыгает раз в секунду вместо плавного хода (localtime() даёт только целые
// секунды).
static void drawClockRing(const DateTime &t) {
    for (int i = 0; i < 60; i++) {
        float a = i * 6.0f * DEG_TO_RAD;
        float dx = sinf(a), dy = -cosf(a);
        int r0 = (i % 5 == 0) ? 104 : 110;
        uint16_t col = (i % 15 == 0) ? C_WHITE : C_GREY;
        spr.drawLine(120 + dx * r0, 120 + dy * r0, 120 + dx * 117, 120 + dy * 117, col);
    }

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    float secFrac = (tv.tv_sec % 60) + tv.tv_usec / 1e6f;
    float minFrac = t.m + secFrac / 60.0f;

    float hourAngle = ((t.h % 12) + minFrac / 60.0f) * 30.0f;
    float minAngle  = minFrac * 6.0f;
    float secAngle  = secFrac * 6.0f;

    auto markAt = [](float angleDeg, uint16_t col) {
        float a = angleDeg * DEG_TO_RAD;
        float dx = sinf(a), dy = -cosf(a);
        spr.drawWideLine(120 + dx * 101, 120 + dy * 101, 120 + dx * 118, 120 + dy * 118, 2.0f, col);
    };
    markAt(hourAngle, C_ACCENT);
    markAt(minAngle,  C_WHITE);
    markAt(secAngle,  C_RED);
}

static void drawCalendarScreen() {
    spr.fillSprite(C_BLACK);
    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextDatum(lgfx::middle_center);

    if (!timeSynced) {
        spr.setTextSize(2);
        spr.setTextColor(C_RED);
        spr.drawString("no time", 120, 110);
        spr.setTextSize(1);
        spr.setTextColor(C_GREY);
        spr.drawString(wifiUp ? "syncing..." : "no WiFi", 120, 140);
        spr.pushSprite(0, 0);
        return;
    }

    DateTime t = nowDT();
    char buf[24];

    drawClockRing(t);

    snprintf(buf, sizeof(buf), "%s %d", MONTHS[t.mon - 1], t.year);
    spr.setTextSize(2);
    spr.setTextColor(C_ACCENT);
    spr.drawString(buf, 120, 52);

    const int cellW = 26, cellH = 19;
    const int gridX = 120 - cellW * 7 / 2;
    const int gridY = 72;

    static const char* WD = "MoTuWeThFrSaSu";
    spr.setTextSize(1);
    for (int i = 0; i < 7; i++) {
        char wd[3] = { WD[i * 2], WD[i * 2 + 1], 0 };
        spr.setTextColor(i >= 5 ? C_RED : C_GREY);
        spr.drawString(wd, gridX + i * cellW + cellW / 2, gridY + 6);
    }

    int firstCol = firstWeekdayOfMonth(t.year, t.mon);
    int dim      = daysInMonth(t.year, t.mon);
    int col = firstCol, row = 0;

    spr.setTextSize(1.4f);   // дата чуть крупнее заголовка-таблицы дней недели
    for (int d = 1; d <= dim; d++) {
        int cx = gridX + col * cellW + cellW / 2;
        int cy = gridY + 18 + row * cellH + cellH / 2;

        if (d == t.day) {
            spr.fillRoundRect(cx - 11, cy - 8, 22, 17, 5, C_ACCENT);
            spr.setTextColor(C_BLACK);
        } else {
            spr.setTextColor(col >= 5 ? C_RED : C_WHITE);
        }
        snprintf(buf, sizeof(buf), "%d", d);
        spr.drawString(buf, cx, cy);

        if (++col > 6) { col = 0; row++; }
    }

    snprintf(buf, sizeof(buf), "%02d:%02d", t.h, t.m);
    spr.setTextSize(2);   // время — тем же размером, что заголовок
    spr.setTextColor(C_ACCENT);
    spr.drawString(buf, 120, 200);

    spr.pushSprite(0, 0);
}

// ─── Список GIF на карте ──────────────────────────────────────────────────────

static void scanDirForGifs(const char* dirPath) {
    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) return;

    File file = dir.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String name = file.name();
            String lower = name; lower.toLowerCase();
            if (lower.endsWith(".gif")) {
                gifFiles.push_back({ String(file.path()), name });
            }
        }
        file.close();
        file = dir.openNextFile();
    }
    dir.close();
}

static void scanGifFiles() {
    gifFiles.clear();
    scanDirForGifs(GIF_DIR);       // сначала /gif
    if (gifFiles.empty()) scanDirForGifs("/");   // иначе корень карты
    Serial.printf("Найдено GIF: %u\n", (unsigned)gifFiles.size());
}

// ─── Файловый доступ AnimatedGIF -> SD (колбэки) ─────────────────────────────
// Декодер запрашивает данные мелкими кусками — читать каждый такой кусок
// напрямую с SD слишком медленно (транзакция на каждый вызов). Вместо этого
// держим окно кэша: реальное чтение с карты — редкое и крупное, вызовы
// декодера обслуживаются из RAM. seek() при этом ленивый — просто двигает
// логическую позицию, реальный f->seek() происходит только при промахе кэша.

static File sdGifFile;

static const size_t GIF_CACHE_SIZE = 8 * 1024;
static uint8_t gifCache[GIF_CACHE_SIZE];
static int32_t cacheStart = -1;   // абсолютная позиция в файле для gifCache[0]
static int32_t cacheLen   = 0;    // сколько валидных байт сейчас в кэше

static void* GIFOpenFile(const char *fname, int32_t *pSize) {
    sdGifFile = SD.open(fname);
    if (sdGifFile) {
        *pSize = sdGifFile.size();
        cacheStart = -1;
        cacheLen = 0;
        return (void*)&sdGifFile;
    }
    return nullptr;
}

static void GIFCloseFile(void *pHandle) {
    File *f = static_cast<File*>(pHandle);
    if (f) f->close();
}

static int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
    File *f = static_cast<File*>(pFile->fHandle);
    int32_t want = iLen;
    // Не читаем самый последний байт файла — после этого у SD-библиотеки
    // перестаёт нормально работать seek() (см. оригинальный пример).
    if (pFile->iPos + want >= pFile->iSize) want = pFile->iSize - pFile->iPos - 1;
    if (want <= 0) return 0;

    int32_t got = 0;
    while (got < want) {
        int32_t filePos = pFile->iPos + got;
        if (cacheStart < 0 || filePos < cacheStart || filePos >= cacheStart + cacheLen) {
            f->seek(filePos);
            cacheStart = filePos;
            cacheLen = f->read(gifCache, GIF_CACHE_SIZE);
            if (cacheLen <= 0) break;
        }
        int32_t offInCache = filePos - cacheStart;
        int32_t chunk = min(cacheLen - offInCache, want - got);
        memcpy(pBuf + got, gifCache + offInCache, chunk);
        got += chunk;
    }
    pFile->iPos += got;
    return got;
}

static int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition) {
    pFile->iPos = iPosition;   // виртуально; реальный seek — лениво, при промахе кэша
    return pFile->iPos;
}

// ─── Колбэк отрисовки строки GIF ──────────────────────────────────────────────
// В отличие от firmware_gif (гифки всегда прогнаны через convert_gif.py и не
// полагаются на disposal=2), тут играются произвольные .gif с карты как есть —
// нужно правильно обработать "restore to background". Два разных случая:
//  1) прозрачные пиксели ВНУТРИ кадра с disposal=2 — гасим в чёрный на лету;
//  2) disposal=2 относится к уже показанному кадру и обязывает очистить ЕГО
//     прямоугольник целиком перед следующим — а колбэк вызывается только для
//     области НОВОГО кадра, так что если тот меньше (картинка "сжалась"),
//     пиксели за его пределами колбэк вообще не тронет. Поэтому прямоугольник
//     предыдущего кадра запоминаем и стираем целиком в начале следующего
//     (pDraw->y == 0 — первая строка нового кадра).

static int  prevFrameX = 0, prevFrameY = 0, prevFrameW = 0, prevFrameH = 0;
static uint8_t prevFrameDisposal = 0;

static void gifDraw(GIFDRAW *pDraw) {
    if (pDraw->y == 0) {
        if (prevFrameDisposal == 2) {
            spr.fillRect(offX + prevFrameX, offY + prevFrameY, prevFrameW, prevFrameH, C_BLACK);
        }
        prevFrameX = pDraw->iX;  prevFrameY = pDraw->iY;
        prevFrameW = pDraw->iWidth; prevFrameH = pDraw->iHeight;
        prevFrameDisposal = pDraw->ucDisposalMethod;
    }

    int width = pDraw->iWidth;
    if (pDraw->iX + width > 240) width = 240 - pDraw->iX;
    if (width <= 0) return;

    int y = pDraw->iY + pDraw->y;
    uint8_t  *src     = pDraw->pPixels;
    uint16_t *palette = pDraw->pPalette;
    static uint16_t line[240];

    if (pDraw->ucDisposalMethod == 2 && pDraw->ucHasTransparency) {
        // "restore to background": не лезем в ucBackground (это индекс в
        // палитре кадра — при локальной палитре может указывать вообще не
        // туда), просто гасим прозрачные пиксели в чёрный — канонический
        // фон везде в проекте.
        uint8_t tr = pDraw->ucTransparent;
        for (int x = 0; x < width; x++)
            line[x] = (src[x] == tr) ? C_BLACK : palette[src[x]];
        spr.pushImage(offX + pDraw->iX, offY + y, width, 1, line);
        return;
    }

    if (pDraw->ucHasTransparency) {
        uint8_t tr = pDraw->ucTransparent;
        int x = 0;
        while (x < width) {
            while (x < width && src[x] == tr) x++;
            int start = x;
            while (x < width && src[x] != tr)
                line[x - start] = palette[src[x]], x++;
            if (x > start)
                spr.pushImage(offX + pDraw->iX + start, offY + y, x - start, 1, line);
        }
    } else {
        for (int x = 0; x < width; x++) line[x] = palette[src[x]];
        spr.pushImage(offX + pDraw->iX, offY + y, width, 1, line);
    }
}

static bool openCurrentGif() {
    if (gifFiles.empty()) return false;

    const GifEntry &entry = gifFiles[gifIndex];
    if (!gif.open(entry.path.c_str(), GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, gifDraw)) {
        Serial.printf("GIF open error for '%s': %d\n", entry.path.c_str(), gif.getLastError());
        return false;
    }

    Serial.printf("GIF[%u/%u]: %s, canvas=%dx%d\n",
                  (unsigned)(gifIndex + 1), (unsigned)gifFiles.size(),
                  entry.name.c_str(), gif.getCanvasWidth(), gif.getCanvasHeight());
    offX = (240 - gif.getCanvasWidth())  / 2;
    offY = (240 - gif.getCanvasHeight()) / 2;
    spr.fillSprite(C_BLACK);
    prevFrameDisposal = 0;   // новый файл — сбросить трекинг предыдущего кадра
    return true;
}

// ─── Монтирование карты ───────────────────────────────────────────────────────
// Сначала штатная скорость, при неудаче — медленная (400 кГц) на случай
// шумных проводов на макетке/длинных джамперов.

static bool trySdBegin() {
    if (SD.begin(SD_CS, sdSPI, 4000000)) return true;
    Serial.println("SD.begin @4MHz failed, retry @400kHz...");
    SD.end();
    delay(50);
    if (SD.begin(SD_CS, sdSPI, 400000)) {
        Serial.println("SD.begin @400kHz OK");
        return true;
    }
    Serial.println("SD.begin failed at both speeds");
    return false;
}

// ─── Кнопки ──────────────────────────────────────────────────────────────────

static bool btnPressed(int pin) {
    if (!digitalRead(pin)) { delay(20); while (!digitalRead(pin)); return true; }
    return false;
}

// Небло­кирующий детектор короткого/долгого нажатия BTN1 — короткая блокирующая
// пауза внутри btnPressed() не даёт измерить длительность удержания, поэтому
// для BTN1 отдельный опрос без задержек.
enum Btn1Event { BTN1_NONE, BTN1_SHORT, BTN1_LONG };

static Btn1Event pollBtn1() {
    static bool wasDown = false;
    static uint32_t downMs = 0;
    static bool longFired = false;
    bool isDown = !digitalRead(BTN1_PIN);   // активный уровень — LOW

    if (isDown && !wasDown) {
        downMs = millis();
        longFired = false;
    } else if (isDown && wasDown && !longFired && millis() - downMs > BTN_LONG_PRESS_MS) {
        longFired = true;
        wasDown = isDown;
        return BTN1_LONG;
    } else if (!isDown && wasDown) {
        wasDown = isDown;
        return longFired ? BTN1_NONE : BTN1_SHORT;
    }
    wasDown = isDown;
    return BTN1_NONE;
}

static bool touchPressed(int pin) {
    static bool lastState = false;
    static uint32_t lastChangeMs = 0;
    bool state = digitalRead(pin) == HIGH;
    uint32_t now = millis();
    if (state != lastState && now - lastChangeMs > 50) {
        lastChangeMs = now;
        lastState = state;
        return state;
    }
    return false;
}

// ─── Setup / Loop ────────────────────────────────────────────────────────────

static bool gifReady = false;

static void nextGif() {
    if (gifFiles.empty()) return;
    gif.close();
    gifIndex = (gifIndex + 1) % gifFiles.size();
    gifReady = openCurrentGif();
}

static void prevGif() {
    if (gifFiles.empty()) return;
    gif.close();
    gifIndex = (gifIndex + gifFiles.size() - 1) % gifFiles.size();
    gifReady = openCurrentGif();
}

// ─── Веб-интерфейс: список / загрузка / удаление GIF по Wi-Fi ────────────────
// Без авторизации — только для домашней сети, не выставляйте наружу в интернет.

static File uploadFile;

static String htmlEscape(const String &s) {
    String out = s;
    out.replace("&", "&amp;");
    out.replace("<", "&lt;");
    out.replace(">", "&gt;");
    return out;
}

static void handleRoot() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                  "<title>MiniScreen GIF</title></head><body>"
                  "<h3>GIF на карте (" + String(gifFiles.size()) + ")</h3><ul>";
    for (auto &e : gifFiles) {
        String n = htmlEscape(e.name);
        html += "<li>" + n +
                " &mdash; <a href='/download?name=" + n + "'>скачать</a>"
                " | <a href='/delete?name=" + n + "' onclick=\"return confirm('Удалить " + n + "?')\">удалить</a></li>";
    }
    html += "</ul><h3>Загрузить новый GIF</h3>"
            "<form method='POST' action='/upload' enctype='multipart/form-data'>"
            "<input type='file' name='data' accept='.gif'> "
            "<input type='submit' value='Загрузить'></form></body></html>";
    server.send(200, "text/html; charset=utf-8", html);
}

static void handleUploadData() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        if (!SD.exists(GIF_DIR)) SD.mkdir(GIF_DIR);
        String path = String(GIF_DIR) + "/" + upload.filename;
        Serial.printf("Upload start: %s\n", path.c_str());
        uploadFile = SD.open(path, FILE_WRITE);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) uploadFile.close();
        Serial.printf("Upload done: %u bytes\n", (unsigned)upload.totalSize);
    }
}

static void handleUploadDone() {
    scanGifFiles();
    server.sendHeader("Location", "/");
    server.send(303);
}

static void handleDownload() {
    if (!server.hasArg("name")) { server.send(400, "text/plain", "no name"); return; }
    String name = server.arg("name");
    String path = String(GIF_DIR) + "/" + name;
    if (!SD.exists(path)) path = "/" + name;
    File f = SD.open(path);
    if (!f) { server.send(404, "text/plain", "not found"); return; }
    server.sendHeader("Content-Disposition", "attachment; filename=" + name);
    server.streamFile(f, "application/octet-stream");
    f.close();
}

static void handleDelete() {
    if (!server.hasArg("name")) { server.send(400, "text/plain", "no name"); return; }
    String name = server.arg("name");
    String path = String(GIF_DIR) + "/" + name;
    if (!SD.exists(path)) path = "/" + name;

    bool wasCurrent = !gifFiles.empty() && gifFiles[gifIndex].name == name;
    SD.remove(path);
    scanGifFiles();

    if (wasCurrent) {
        gif.close();
        gifIndex = 0;
        gifReady = gifFiles.empty() ? false : openCurrentGif();
    } else if (!gifFiles.empty() && gifIndex >= gifFiles.size()) {
        gifIndex = 0;
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

static void setupWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
    server.on("/download", HTTP_GET, handleDownload);
    server.on("/delete", HTTP_GET, handleDelete);
    server.begin();
    Serial.println("Web server started on port 80");
}

void setup() {
    Serial.begin(115200);
    Serial.println("GIF player (MicroSD)");

    pinMode(BTN1_PIN, INPUT_PULLUP);
    pinMode(BTN2_PIN, INPUT_PULLUP);
    pinMode(TOUCH_PIN, INPUT_PULLDOWN);

    tft.init();
    tft.setRotation(0);
    tft.setBrightness(220);
    tft.fillScreen(C_BLACK);

    spr.setColorDepth(16);
    spr.setSwapBytes(true);   // палитра GIF little-endian, как в firmware_gif
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

    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    gif.begin(GIF_PALETTE_RGB565_LE);
    statusScreen("SD card", "checking...", C_CYAN);

    wifiUp = connectWiFiOnce();
    lastWifiAttemptMs = millis();
    if (wifiUp) setupWebServer();
}

void loop() {
    switch (pollBtn1()) {
        case BTN1_SHORT: nextGif(); break;
        case BTN1_LONG:  prevGif(); break;
        default: break;
    }
    if (touchPressed(TOUCH_PIN)) {
        screenMode = (ScreenMode)((screenMode + 1) % 3);
        lastModeDrawMs = 0;   // сразу перерисовать новый экран, не дожидаясь таймера
    }
    if (btnPressed(BTN2_PIN)) {
        brightnessIdx = (brightnessIdx + 1) % BRIGHTNESS_LEVELS_COUNT;
        tft.setBrightness(BRIGHTNESS_LEVELS[brightnessIdx]);
    }

    if (!wifiUp && millis() - lastWifiAttemptMs >= WIFI_RETRY_MS) {
        lastWifiAttemptMs = millis();
        wifiUp = connectWiFiOnce();
        if (wifiUp) setupWebServer();
    }
    if (wifiUp) { server.handleClient(); syncTimeIfNeeded(); }

    if (!sdMounted) {
        if (millis() - lastSdAttemptMs < SD_RETRY_MS) { delay(20); return; }
        lastSdAttemptMs = millis();
        SD.end();
        if (!trySdBegin()) {
            statusScreen("NO SD CARD", "insert card...", C_RED);
            return;
        }
        sdMounted = true;
        scanGifFiles();
        if (gifFiles.empty()) {
            statusScreen("NO GIF FILES", "put .gif in /gif", C_RED);
            sdMounted = false;   // продолжаем ждать/пересканировать
            return;
        }
        gifIndex = 0;
        gifReady = openCurrentGif();
        if (!gifReady) {
            statusScreen("GIF ERROR", gifFiles[0].name.c_str(), C_RED);
            return;
        }
    }

    // Карту вынули на горячую
    if (SD.cardType() == CARD_NONE) {
        sdMounted = false;
        gifReady = false;
        SD.end();
        return;
    }

    // Экраны сети/календаря — гифка на это время не проигрывается вовсе
    if (screenMode == MODE_NETWORK) {
        if (millis() - lastModeDrawMs >= 500) { drawNetworkScreen(); lastModeDrawMs = millis(); }
        delay(20);
        return;
    }
    if (screenMode == MODE_CALENDAR) {
        if (millis() - lastModeDrawMs >= 40) { drawCalendarScreen(); lastModeDrawMs = millis(); }
        delay(5);
        return;
    }

    if (!gifReady) { delay(10); return; }

    int delayMs = 0;
    if (!gif.playFrame(false, &delayMs)) {
        nextGif();
        return;
    }
    spr.pushSprite(0, 0);

    static uint32_t lastFrame = 0;
    uint32_t elapsed = millis() - lastFrame;
    if (elapsed < (uint32_t)delayMs) delay(delayMs - elapsed);
    lastFrame = millis();
}
