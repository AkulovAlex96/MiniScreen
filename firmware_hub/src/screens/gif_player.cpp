#include <vector>
#include "../core/screen.h"
#include "../core/gfx.h"
#include "../core/storage.h"
#include "../core/decoders.h"

// ─── GIF-плеер с MicroSD (порт firmware_gif_sd) ───────────────────────────────
// Гифки читаются файлами с карты (папка /gif, иначе корень). Управление
// файлами — в общем webui (/files); после загрузки/удаления ядро дёргает
// onConfigChanged() — пересканируем.
//
// BTN1 short — следующий GIF, long — предыдущий

namespace gifplay {

static const char* GIF_DIR = "/gif";
static const uint32_t SD_RETRY_MS = 1500;

struct GifEntry { String path; String name; };
static std::vector<GifEntry> gifFiles;
static size_t gifIndex = 0;

static int offX = 0, offY = 0;
static bool gifReady = false;
static uint32_t lastSdAttemptMs = 0;
static uint32_t nextFrameDueMs = 0;
static bool rescanPending = false;

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
// Декодер запрашивает данные мелкими кусками — держим окно кэша: реальное
// чтение с карты редкое и крупное, seek() ленивый (двигает только логическую
// позицию, реальный f->seek() — при промахе кэша).

static File sdGifFile;

static const size_t GIF_CACHE_SIZE = 8 * 1024;
static uint8_t gifCache[GIF_CACHE_SIZE];
static int32_t cacheStart = -1;   // абсолютная позиция в файле для gifCache[0]
static int32_t cacheLen   = 0;

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
    // перестаёт нормально работать seek() (см. оригинальный пример AnimatedGIF)
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
    pFile->iPos = iPosition;   // виртуально; реальный seek — при промахе кэша
    return pFile->iPos;
}

// ─── Колбэк отрисовки строки GIF ──────────────────────────────────────────────
// Играются произвольные .gif как есть — нужно правильно обработать
// disposal=2 ("restore to background"): прозрачные пиксели внутри кадра гасим
// в чёрный, а прямоугольник предыдущего кадра стираем целиком в начале
// следующего (колбэк вызывается только для области НОВОГО кадра).

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
        // Не лезем в ucBackground (при локальной палитре может указывать не
        // туда) — просто гасим прозрачные пиксели в чёрный
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
    nextFrameDueMs = 0;
    return true;
}

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

} // namespace gifplay

class GifPlayerScreen : public Screen {
public:
    const char* id() const override    { return "gif"; }
    const char* title() const override { return "GIF player"; }

    void onEnter() override {
        using namespace gifplay;
        gifReady = false;
        lastSdAttemptMs = 0;   // монтировать/сканировать при первом tick
        rescanPending = true;
    }

    void onExit() override {
        using namespace gifplay;
        if (gifReady) gif.close();
        gifReady = false;
    }

    void tick(uint32_t nowMs) override {
        using namespace gifplay;

        if (!sdMounted() || rescanPending) {
            if (lastSdAttemptMs && nowMs - lastSdAttemptMs < SD_RETRY_MS) return;
            lastSdAttemptMs = nowMs;
            if (gifReady) { gif.close(); gifReady = false; }
            if (!sdMountIfNeeded()) {
                statusScreen("NO SD CARD", "insert card...", C_RED);
                return;
            }
            scanGifFiles();
            rescanPending = false;
            if (gifFiles.empty()) {
                statusScreen("NO GIF FILES", "put .gif in /gif", C_RED);
                rescanPending = true;   // продолжаем пересканировать
                return;
            }
            if (gifIndex >= gifFiles.size()) gifIndex = 0;
            gifReady = openCurrentGif();
            if (!gifReady) {
                statusScreen("GIF ERROR", gifFiles[gifIndex].name.c_str(), C_RED);
                return;
            }
        }

        // Карту вынули на горячую
        if (SD.cardType() == CARD_NONE) {
            sdUnmount();
            gifReady = false;
            return;
        }

        if (!gifReady) return;
        if (nextFrameDueMs && nowMs < nextFrameDueMs) return;

        int delayMs = 0;
        if (!gif.playFrame(false, &delayMs)) {
            nextGif();
            return;
        }
        spr.pushSprite(0, 0);
        nextFrameDueMs = millis() + max(delayMs, 2);
    }

    uint32_t frameDelayMs() const override { return 2; }   // пейсинг — по delay кадра GIF

    void onButton(BtnEvent ev) override {
        if (ev == EV_BTN1_SHORT) gifplay::nextGif();
        if (ev == EV_BTN1_LONG)  gifplay::prevGif();
    }

    void onConfigChanged() override {   // веб загрузил/удалил файл
        gifplay::rescanPending = true;
        gifplay::lastSdAttemptMs = 0;
    }
};

Screen* gifPlayerScreen() {
    static GifPlayerScreen s;
    return &s;
}
