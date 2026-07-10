#include "../core/screen.h"
#include "../core/gfx.h"
#include "../core/storage.h"

// ─── Инфо о MicroSD (порт firmware_sdcard) ────────────────────────────────────
// Объём/занято (кольцевой прогресс), количество файлов/папок/гифок.
//
// BTN1 short — пересканировать карту прямо сейчас

namespace sdinfo {

static const uint32_t RETRY_INTERVAL_MS = 1500;
static const int      SCAN_DEPTH_LIMIT  = 6;   // защита от рекурсии по вложенным папкам

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

// FATFS отдаёт totalBytes()/usedBytes() напрямую, а вот количество файлов
// приходится считать самим — рекурсивный обход.
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

static void drawCardInfo() {
    spr.fillSprite(C_BLACK);

    float pct = totalBytes > 0 ? (float)usedBytes / (float)totalBytes * 100.0f : 0.0f;
    uint16_t ringCol = pct > 90.0f ? C_RED : (pct > 70.0f ? C_YELLOW : C_GREEN);

    spr.fillArc(120, 120, 118, 100, 0.0f, 360.0f, C_DGREY);
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
}

} // namespace sdinfo

class SdInfoScreen : public Screen {
    bool forceRescan = true;
    uint32_t lastAttemptMs = 0;

public:
    const char* id() const override    { return "sd_info"; }
    const char* title() const override { return "SD info"; }

    void onEnter() override {
        forceRescan = true;
        lastAttemptMs = 0;
    }

    bool tick(uint32_t nowMs) override {
        using namespace sdinfo;

        if (!sdMounted()) {
            if (lastAttemptMs && nowMs - lastAttemptMs < RETRY_INTERVAL_MS) return false;
            lastAttemptMs = nowMs;
            if (!sdMountIfNeeded()) {
                statusScreen("NO SD CARD", "insert card...", C_RED);
                return true;
            }
            forceRescan = true;
        }

        // Пропала карта (вынули на горячую)
        if (SD.cardType() == CARD_NONE) {
            sdUnmount();
            return false;
        }

        if (forceRescan) {
            forceRescan = false;
            statusScreen("Scanning", "reading files...", C_CYAN);
            spr.pushSprite(0, 0);   // впереди блокирующий обход карты — статус сразу
            rescan();
            drawCardInfo();
            return true;
        }
        return false;   // кадр статичен до следующего рескана
    }

    uint32_t frameDelayMs() const override { return 200; }

    void onButton(BtnEvent ev) override {
        if (ev == EV_BTN1_SHORT) forceRescan = true;
    }
};

Screen* sdInfoScreen() {
    static SdInfoScreen s;
    return &s;
}
