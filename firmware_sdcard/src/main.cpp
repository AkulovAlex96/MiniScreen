#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <LovyanGFX.hpp>

// ═══ Инфо о MicroSD на GC9A01 240x240 ═════════════════════════════════════════
// Отдельная SPI-шина (HSPI/SPI3), НЕ общая с дисплеем (SPI2) — драйвер
// LovyanGFX держит SPI2 монопольно, делить его с библиотекой SD нельзя без
// низкоуровневой возни с esp-idf, поэтому проще и надёжнее взять второй
// физический SPI-хост.
//
// CATALEX MicroSD Card Adapter:
//   GND->GND  VCC->5V(!)  MISO->GPIO41  MOSI->GPIO40  SCK->GPIO39  CS->GPIO38
// VCC именно 5V — на плате свой AMS1117-3.3 с просадкой ~1В, от 3.3V на
// выходе будет ~2.0-2.3V, SD-карте не хватает для стабильной инициализации.
// Буфер линий CS/SCK/MOSI/MISO сидит на регулированных 3.3V, так что для
// ESP32 они безопасны независимо от того, что подано на VCC.
//
// BTN1 (GPIO1) — пересканировать карту прямо сейчас
// BTN2 (GPIO2) — подсветка вкл/выкл

#define SD_CS   38
#define SD_SCK  39
#define SD_MOSI 40
#define SD_MISO 41

#define TOUCH_PIN 15   // сенсорная кнопка (TTP223 OUT), не путать с CST816S RST на будущей плате

static const uint32_t RETRY_INTERVAL_MS = 1500;   // как часто пробовать примонтировать, если карты нет
static const int      SCAN_DEPTH_LIMIT  = 6;       // защита от бесконечной рекурсии по вложенным папкам

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
static SPIClass sdSPI(HSPI);   // HSPI = SPI3 на S3 — независимая от дисплея шина

#define BTN1_PIN 1
#define BTN2_PIN 2

static const uint16_t C_BLACK  = 0x0000;
static const uint16_t C_WHITE  = 0xFFFF;
static const uint16_t C_RED    = 0xF800;
static const uint16_t C_YELLOW = 0xFFE0;
static const uint16_t C_GREEN  = 0x07E0;
static const uint16_t C_CYAN   = 0x07FF;
static const uint16_t C_GREY   = 0x7BEF;
static const uint16_t C_DGREY  = 0x39C7;

static bool blOn = true;
static bool cardMounted = false;
static uint32_t lastAttemptMs = 0;
static bool forceRescan = true;

static uint64_t totalBytes = 0, usedBytes = 0;
static uint32_t fileCount = 0, gifCount = 0, dirCount = 0;
static sdcard_type_t cardType = CARD_NONE;

static const char* cardTypeStr(sdcard_type_t t) {
    switch (t) {
        case CARD_MMC:  return "MMC";
        case CARD_SD:   return "SDSC";
        case CARD_SDHC: return "SDHC/XC";
        default:        return "unknown";
    }
}

// Рекурсивный обход дерева файлов — считает файлы/папки/гифки.
// FATFS отдаёт totalBytes()/usedBytes() напрямую (без обхода), а вот
// количество файлов приходится считать самим.
static void scanDir(File dir, int depthLeft) {
    File file = dir.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            dirCount++;
            if (depthLeft > 0) scanDir(file, depthLeft - 1);
        } else {
            fileCount++;
            String name = file.name();
            name.toLowerCase();
            if (name.endsWith(".gif")) gifCount++;
        }
        file.close();
        file = dir.openNextFile();
    }
}

static void rescan() {
    fileCount = 0; gifCount = 0; dirCount = 0;
    File root = SD.open("/");
    if (root) {
        scanDir(root, SCAN_DEPTH_LIMIT);
        root.close();
    }
    totalBytes = SD.totalBytes();
    usedBytes  = SD.usedBytes();
    cardType   = SD.cardType();
    Serial.printf("SD: %s, %.2f/%.2f GB, %u files (%u gif) in %u dirs\n",
                  cardTypeStr(cardType), usedBytes / 1e9, totalBytes / 1e9,
                  fileCount, gifCount, dirCount);
}

// ─── Отрисовка ────────────────────────────────────────────────────────────────

static void drawStatus(const char* line1, const char* line2, uint16_t col) {
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

static void drawCardInfo() {
    spr.fillSprite(C_BLACK);

    float pct = totalBytes > 0 ? (float)usedBytes / (float)totalBytes * 100.0f : 0.0f;
    uint16_t ringCol = pct > 90.0f ? C_RED : (pct > 70.0f ? C_YELLOW : C_GREEN);

    spr.fillArc(120, 120, 118, 100, 0.0f, 360.0f, C_DGREY);      // фон кольца
    if (pct > 0.1f) spr.fillArc(120, 120, 118, 100, 0.0f, pct * 3.6f, ringCol);

    spr.setFont(&lgfx::fonts::Font4);
    spr.setTextDatum(lgfx::middle_center);
    spr.setTextColor(C_WHITE, C_BLACK);
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f%%", pct);
    spr.drawString(buf, 120, 92);

    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextSize(1);
    spr.setTextColor(C_GREY, C_BLACK);
    snprintf(buf, sizeof(buf), "%.1f / %.1f GB", usedBytes / 1e9, totalBytes / 1e9);
    spr.drawString(buf, 120, 125);

    spr.setTextColor(C_WHITE, C_BLACK);
    snprintf(buf, sizeof(buf), "%u files", fileCount);
    spr.drawString(buf, 120, 148);

    spr.setTextColor(C_GREY, C_BLACK);
    snprintf(buf, sizeof(buf), "%u dirs, %u gif", dirCount, gifCount);
    spr.drawString(buf, 120, 165);

    spr.setTextColor(C_CYAN, C_BLACK);
    spr.drawString(cardTypeStr(cardType), 120, 195);

    spr.pushSprite(0, 0);
}

// ─── Монтирование карты ───────────────────────────────────────────────────────
// Сначала пробуем штатную скорость, при неудаче — медленную (400 кГц, как в
// самом начале SD-протокола) на случай шумных проводов на макетке/длинных
// джамперов — частая причина "не видит карту" при рабочей карте и верной схеме.

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

// Сенсорная кнопка (TTP223-типа): в отличие от BTN1/BTN2 сама активно
// драйвит пин, HIGH = касание. Пул-даун + edge-детект по фронту, без
// блокирующего delay (чтобы не мешать опросу обычных кнопок в том же loop()).
static bool touchPressed(int pin) {
    static bool lastState = false;
    static uint32_t lastChangeMs = 0;
    bool state = digitalRead(pin) == HIGH;
    uint32_t now = millis();
    if (state != lastState && now - lastChangeMs > 50) {
        lastChangeMs = now;
        lastState = state;
        return state;   // true только в момент касания, не на отпускание
    }
    return false;
}

// ─── Setup / Loop ────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("SD card info");

    pinMode(BTN1_PIN, INPUT_PULLUP);
    pinMode(BTN2_PIN, INPUT_PULLUP);
    pinMode(TOUCH_PIN, INPUT_PULLDOWN);

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

    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    drawStatus("SD card", "checking...", C_CYAN);
}

void loop() {
    if (btnPressed(BTN1_PIN)) forceRescan = true;
    if (touchPressed(TOUCH_PIN)) forceRescan = true;   // сенсорная кнопка — тот же рефреш, что BTN1
    if (btnPressed(BTN2_PIN)) {
        blOn = !blOn;
        tft.setBrightness(blOn ? 220 : 0);
    }

    if (!cardMounted) {
        if (forceRescan || millis() - lastAttemptMs >= RETRY_INTERVAL_MS) {
            forceRescan = false;
            lastAttemptMs = millis();
            SD.end();
            if (trySdBegin()) {
                cardMounted = true;
                drawStatus("Scanning", "reading files...", C_CYAN);
                rescan();
                drawCardInfo();
            } else {
                drawStatus("NO SD CARD", "insert card...", C_RED);
            }
        }
    } else {
        if (forceRescan) {
            forceRescan = false;
            drawStatus("Scanning", "reading files...", C_CYAN);
            rescan();
            drawCardInfo();
        }
        // Пропала карта (вынули на горячую) — SD.cardType() возвращает CARD_NONE
        if (SD.cardType() == CARD_NONE) {
            cardMounted = false;
            SD.end();
        }
    }

    delay(200);
}
