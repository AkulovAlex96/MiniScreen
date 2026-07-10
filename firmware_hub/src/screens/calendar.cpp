#include "../core/screen.h"
#include "../core/gfx.h"
#include "../core/net.h"
#include "../core/timesvc.h"

// ─── Календарь с часовым кольцом (вариант из firmware_gif_sd) ─────────────────
// Кольцо насечек по контуру + 3 толстые метки — текущие час/минута/секунда.
// Секунда с дробной частью, иначе метка дискретно прыгает раз в секунду.

class CalendarScreen : public Screen {
    void drawClockRing(const DateTime& t) {
        for (int i = 0; i < 60; i++) {
            float a = i * 6.0f * DEG_TO_RAD;
            float dx = sinf(a), dy = -cosf(a);
            int r0 = (i % 5 == 0) ? 104 : 110;
            uint16_t col = (i % 15 == 0) ? C_WHITE : C_GREY;
            spr.drawLine(120 + dx * r0, 120 + dy * r0, 120 + dx * 117, 120 + dy * 117, col);
        }

        float secFrac = t.s + timeSecFrac();
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

public:
    const char* id() const override    { return "calendar"; }
    const char* title() const override { return "Calendar"; }

    bool tick(uint32_t) override {
        spr.fillSprite(C_BLACK);
        spr.setFont(&lgfx::fonts::Font0);
        spr.setTextDatum(lgfx::middle_center);

        if (!timeSynced()) {
            spr.setTextSize(2);
            spr.setTextColor(C_RED);
            spr.drawString("no time", 120, 110);
            spr.setTextSize(1);
            spr.setTextColor(C_GREY);
            spr.drawString(netUp() ? "syncing..." : "no WiFi", 120, 140);
            return true;
        }

        DateTime t = timeNow();
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

        spr.setTextSize(1.4f);   // дата чуть крупнее шапки дней недели
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
        spr.setTextSize(2);
        spr.setTextColor(C_ACCENT);
        spr.drawString(buf, 120, 200);

        return true;
    }
};

Screen* calendarScreen() {
    static CalendarScreen s;
    return &s;
}
