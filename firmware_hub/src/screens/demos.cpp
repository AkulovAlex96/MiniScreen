#include "../core/screen.h"
#include "../core/gfx.h"

// ─── Демо-виджеты из firmware_widgets: 5 экранов в одном файле ────────────────
// Ring loader, progress bar, gauge, matrix, starfield. Мелкие и без внешних
// зависимостей — держим вместе, чтобы не плодить файлы-обёртки.

// ═══ Ring loader ═══════════════════════════════════════════════════════════════

class RingLoaderScreen : public Screen {
public:
    const char* id() const override    { return "loader_ring"; }
    const char* title() const override { return "Ring loader"; }

    bool tick(uint32_t nowMs) override {
        spr.fillSprite(C_BLACK);

        // 0→100% по синусу, цикл ~6 сек
        float phase = nowMs / 3000.0f * PI;
        float p = (1.0f - cosf(phase)) * 50.0f;

        spr.fillArc(120, 120, 112, 100, 0.0f, 360.0f, C_DGREY);
        uint16_t col = hsv565((uint8_t)(p * 0.85f), 240, 255);
        arcSeg(112, 100, -90.0f, -90.0f + p * 3.6f, col);

        float aHead = (-90.0f + p * 3.6f) * DEG_TO_RAD;
        spr.fillCircle(120 + cosf(aHead) * 106, 120 + sinf(aHead) * 106, 7, C_WHITE);

        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", (int)p);
        spr.setFont(&lgfx::fonts::Font0);
        spr.setTextDatum(lgfx::middle_center);
        spr.setTextSize(4);
        spr.setTextColor(C_WHITE);
        spr.drawString(buf, 120, 112);

        spr.setTextSize(1);
        spr.setTextColor(C_GREY);
        spr.drawString(p > 99.0f ? "complete" : "loading...", 120, 152);

        return true;
    }
};

// ═══ Progress bar ══════════════════════════════════════════════════════════════

class ProgressBarScreen : public Screen {
public:
    const char* id() const override    { return "loader_bar"; }
    const char* title() const override { return "Progress bar"; }

    bool tick(uint32_t nowMs) override {
        spr.fillSprite(C_BLACK);

        float phase = nowMs / 3000.0f * PI;
        float p = (1.0f - cosf(phase)) * 50.0f;

        const int bx = 45, by = 112, bw = 150, bh = 18;
        int fillW = (int)(bw * p / 100.0f);

        spr.drawRoundRect(bx - 2, by - 2, bw + 4, bh + 4, 9, C_GREY);
        if (fillW > 4)
            spr.fillRoundRect(bx, by, fillW, bh, 7, hsv565((uint8_t)(p * 0.85f), 240, 255));

        // Бегущий блик по заполненной части
        int shineW = 18;
        int shineX = bx + (int)(fmodf(nowMs / 12.0f, (float)(bw + shineW))) - shineW;
        int sLeft  = max(shineX, bx);
        int sRight = min(shineX + shineW, bx + fillW);
        if (sRight > sLeft)
            spr.fillRect(sLeft, by + 3, sRight - sLeft, bh - 6, rgb565(255, 255, 255));

        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", (int)p);
        spr.setFont(&lgfx::fonts::Font0);
        spr.setTextDatum(lgfx::middle_center);
        spr.setTextSize(3);
        spr.setTextColor(C_WHITE);
        spr.drawString(buf, 120, 82);

        int dots = (nowMs / 400) % 4;
        char dbuf[16] = "Downloading";
        for (int i = 0; i < dots; i++) strcat(dbuf, ".");
        spr.setTextSize(1);
        spr.setTextColor(C_GREY);
        spr.drawString(dbuf, 120, 156);

        return true;
    }
};

// ═══ Gauge ═════════════════════════════════════════════════════════════════════

class GaugeScreen : public Screen {
public:
    const char* id() const override    { return "gauge"; }
    const char* title() const override { return "Gauge"; }

    bool tick(uint32_t nowMs) override {
        spr.fillSprite(C_BLACK);

        // Значение — плавный «случайный» дрейф
        float tt = nowMs / 1000.0f;
        float v = 50.0f + 35.0f * sinf(tt * 0.9f) + 15.0f * sinf(tt * 2.3f);
        v = constrain(v, 0.0f, 100.0f);

        const float A0 = 135.0f, SPAN = 270.0f;

        arcSeg(112, 102, A0, A0 + SPAN, C_DGREY);
        arcSeg(112, 102, A0, A0 + SPAN * v / 100.0f,
               hsv565((uint8_t)(85 - v * 0.85f), 240, 255));

        spr.setFont(&lgfx::fonts::Font0);
        spr.setTextSize(1);
        spr.setTextDatum(lgfx::middle_center);
        for (int i = 0; i <= 10; i++) {
            float a = (A0 + SPAN * i / 10.0f) * DEG_TO_RAD;
            float dx = cosf(a), dy = sinf(a);
            spr.drawLine(120 + dx * 98, 120 + dy * 98, 120 + dx * 90, 120 + dy * 90,
                         (i % 5 == 0) ? C_WHITE : C_GREY);
            if (i % 5 == 0) {
                char lbl[4];
                snprintf(lbl, sizeof(lbl), "%d", i * 10);
                spr.setTextColor(C_GREY);
                spr.drawString(lbl, 120 + dx * 78, 120 + dy * 78);
            }
        }

        float aN = (A0 + SPAN * v / 100.0f) * DEG_TO_RAD;
        // drawHand использует угол от 12 часов — пересчёт: rad от +x → от вертикали
        drawHand(aN + PI / 2.0f, 84, 3.0f, 16, C_RED);
        spr.fillCircle(120, 120, 7, C_WHITE);
        spr.fillCircle(120, 120, 4, C_RED);

        char buf[8];
        snprintf(buf, sizeof(buf), "%d", (int)v);
        spr.setTextSize(3);
        spr.setTextColor(C_WHITE);
        spr.drawString(buf, 120, 172);
        spr.setTextSize(1);
        spr.setTextColor(C_GREY);
        spr.drawString("km/h", 120, 196);

        return true;
    }
};

// ═══ Matrix ════════════════════════════════════════════════════════════════════
// Стейт-фул: спрайт не очищается между кадрами — onEnter обязателен

class MatrixScreen : public Screen {
    static const int COLS = 20, ROWS = 15;   // ячейка 12x16 (шрифт 6x8 x2)
    int8_t  head[COLS];
    int8_t  len[COLS];
    uint8_t divs[COLS];   // делитель скорости
    uint8_t cnt[COLS];

    void resetCol(int c, bool randomY) {
        head[c] = randomY ? random(-ROWS, ROWS) : -random(1, 8);
        len[c]  = random(4, 12);
        divs[c] = random(1, 4);
        cnt[c]  = 0;
    }

    void cell(int c, int row, char ch, uint16_t col) {
        if (row < 0 || row >= ROWS) return;
        int x = c * 12, y = row * 16;
        spr.fillRect(x, y, 12, 16, C_BLACK);
        if (ch) {
            spr.setCursor(x, y);
            spr.setTextColor(col);
            spr.print(ch);
        }
    }

    static char randGlyph() { return (char)random(33, 127); }

public:
    const char* id() const override    { return "matrix"; }
    const char* title() const override { return "Matrix"; }

    void onEnter() override {
        spr.fillSprite(C_BLACK);
        for (int c = 0; c < COLS; c++) resetCol(c, true);
    }

    bool tick(uint32_t) override {
        spr.setFont(&lgfx::fonts::Font0);
        spr.setTextSize(2);
        spr.setTextDatum(lgfx::top_left);

        for (int c = 0; c < COLS; c++) {
            if (++cnt[c] < divs[c]) continue;
            cnt[c] = 0;

            int h = ++head[c];
            cell(c, h,     randGlyph(), rgb565(220, 255, 220));   // голова — почти белая
            cell(c, h - 1, randGlyph(), C_GREEN);                 // за ней — ярко-зелёный
            cell(c, h - len[c] + 1, randGlyph(), rgb565(0, 90, 0));   // хвост тускнеет
            cell(c, h - len[c], 0, C_BLACK);                      // конец хвоста — стереть

            if (h - len[c] > ROWS) resetCol(c, false);
        }

        return true;
    }
};

// ═══ Starfield ═════════════════════════════════════════════════════════════════

class StarfieldScreen : public Screen {
    static const int NUM_STARS = 90;
    struct Star { float x, y, z; };
    Star stars[NUM_STARS];

    void reset(Star& s) {
        s.x = random(-1000, 1001) / 1000.0f;
        s.y = random(-1000, 1001) / 1000.0f;
        s.z = random(300, 1000) / 1000.0f;
    }

public:
    const char* id() const override    { return "starfield"; }
    const char* title() const override { return "Starfield"; }

    void onEnter() override {
        for (auto& s : stars) reset(s);
    }

    bool tick(uint32_t) override {
        spr.fillSprite(C_BLACK);

        for (auto& s : stars) {
            s.z -= 0.018f;
            if (s.z <= 0.05f) { reset(s); s.z = 1.0f; }

            int px = 120 + (int)(s.x / s.z * 110.0f);
            int py = 120 + (int)(s.y / s.z * 110.0f);
            if (px < 0 || px > 239 || py < 0 || py > 239) { reset(s); s.z = 1.0f; continue; }

            uint8_t b = (uint8_t)constrain((1.1f - s.z) * 255.0f, 40.0f, 255.0f);
            int sz = s.z < 0.25f ? 3 : (s.z < 0.55f ? 2 : 1);
            spr.fillRect(px, py, sz, sz, rgb565(b, b, b));
        }

        return true;
    }
};

Screen* ringLoaderScreen()  { static RingLoaderScreen s;  return &s; }
Screen* progressBarScreen() { static ProgressBarScreen s; return &s; }
Screen* gaugeScreen()       { static GaugeScreen s;       return &s; }
Screen* matrixScreen()      { static MatrixScreen s;      return &s; }
Screen* starfieldScreen()   { static StarfieldScreen s;   return &s; }
