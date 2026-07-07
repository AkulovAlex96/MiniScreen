#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <LovyanGFX.hpp>
#include <AnimatedGIF.h>
#include <vector>

// ═══ Проигрывание GIF с MicroSD на GC9A01 240x240 ═════════════════════════════
// В отличие от firmware_gif (гифки зашиты во flash как C-массивы), тут гифки
// читаются файлами прямо с карты — никакого convert_gif.py/пересборки, просто
// скинуть .gif на карту в папку /gif (или в корень).
//
// MicroSD — отдельная SPI-шина (HSPI/SPI3), не общая с дисплеем: та же
// разводка, что в firmware_sdcard.
//   GND->GND  VCC->3V3  MISO->GPIO46  MOSI->GPIO40  SCK->GPIO39  CS->GPIO38
//
// BTN1 (GPIO1) — пауза/продолжить
// BTN2 (GPIO2) — подсветка вкл/выкл
// Сенсорная кнопка (GPIO15, опционально) — следующий GIF

#define SD_CS   38
#define SD_SCK  39
#define SD_MOSI 40
#define SD_MISO 46

#define TOUCH_PIN 15

static const char*    GIF_DIR           = "/gif";   // сначала ищем тут, потом в корне
static const uint32_t SD_RETRY_MS       = 1500;

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

#define BTN1_PIN 1
#define BTN2_PIN 2

static const uint16_t C_BLACK = 0x0000;
static const uint16_t C_WHITE = 0xFFFF;
static const uint16_t C_RED   = 0xF800;
static const uint16_t C_CYAN  = 0x07FF;
static const uint16_t C_GREY  = 0x7BEF;

static bool paused      = false;
static bool blOn        = true;
static bool sdMounted   = false;
static uint32_t lastSdAttemptMs = 0;

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
// Паттерн из официального примера AnimatedGIF/examples/ESP32-LGFX-SDCard-GifPlayer.

static File sdGifFile;

static void* GIFOpenFile(const char *fname, int32_t *pSize) {
    sdGifFile = SD.open(fname);
    if (sdGifFile) {
        *pSize = sdGifFile.size();
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
    int32_t iBytesRead = iLen;
    // Примечание из оригинального примера: если дочитать файл до последнего
    // байта, seek() у SD-библиотеки перестаёт нормально работать — оставляем
    // тот же зазор в 1 байт.
    if ((pFile->iSize - pFile->iPos) < iLen) iBytesRead = pFile->iSize - pFile->iPos - 1;
    if (iBytesRead <= 0) return 0;
    iBytesRead = f->read(pBuf, iBytesRead);
    pFile->iPos = f->position();
    return iBytesRead;
}

static int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition) {
    File *f = static_cast<File*>(pFile->fHandle);
    f->seek(iPosition);
    pFile->iPos = (int32_t)f->position();
    return pFile->iPos;
}

// ─── Колбэк отрисовки строки GIF (как в firmware_gif) ────────────────────────

static void gifDraw(GIFDRAW *pDraw) {
    int width = pDraw->iWidth;
    if (pDraw->iX + width > 240) width = 240 - pDraw->iX;
    if (width <= 0) return;

    int y = pDraw->iY + pDraw->y;
    uint8_t  *src     = pDraw->pPixels;
    uint16_t *palette = pDraw->pPalette;
    static uint16_t line[240];

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
    return true;
}

// ─── Кнопки ──────────────────────────────────────────────────────────────────

static bool btnPressed(int pin) {
    if (!digitalRead(pin)) { delay(20); while (!digitalRead(pin)); return true; }
    return false;
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
}

void loop() {
    if (btnPressed(BTN1_PIN)) paused = !paused;
    if (touchPressed(TOUCH_PIN)) nextGif();
    if (btnPressed(BTN2_PIN)) {
        blOn = !blOn;
        tft.setBrightness(blOn ? 220 : 0);
    }

    if (!sdMounted) {
        if (millis() - lastSdAttemptMs < SD_RETRY_MS) { delay(20); return; }
        lastSdAttemptMs = millis();
        SD.end();
        if (!SD.begin(SD_CS, sdSPI)) {
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

    if (paused || !gifReady) { delay(10); return; }

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
