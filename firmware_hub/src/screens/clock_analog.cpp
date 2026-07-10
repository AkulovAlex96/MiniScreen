#include "../core/screen.h"
#include "../core/gfx.h"
#include "../core/net.h"
#include "../core/timesvc.h"

// ─── Аналоговые часы (из firmware_widgets, время — NTP вместо компилятора) ────

class ClockAnalogScreen : public Screen {
public:
    const char* id() const override    { return "clock_analog"; }
    const char* title() const override { return "Analog clock"; }

    bool tick(uint32_t) override {
        if (!timeSynced()) {
            statusScreen("no time", netUp() ? "syncing..." : "no WiFi", C_RED);
            return true;
        }
        DateTime t = timeNow();
        float secFrac = timeSecFrac();

        spr.fillSprite(C_BLACK);

        // Циферблат
        spr.fillArc(120, 120, 119, 115, 0.0f, 360.0f, C_DGREY);
        for (int i = 0; i < 60; i++) {
            float a = i * 6.0f * DEG_TO_RAD;
            float dx = sinf(a), dy = -cosf(a);
            int r1 = (i % 5 == 0) ? 100 : 108;
            uint16_t col = (i % 15 == 0) ? C_WHITE : C_GREY;
            spr.drawLine(120 + dx * r1, 120 + dy * r1,
                         120 + dx * 113, 120 + dy * 113, col);
        }

        spr.setFont(&lgfx::fonts::Font0);
        spr.setTextSize(2);
        spr.setTextColor(C_WHITE);
        spr.setTextDatum(lgfx::middle_center);
        spr.drawString("12", 120, 34);
        spr.drawString("3",  206, 120);
        spr.drawString("6",  120, 206);
        spr.drawString("9",   34, 120);

        // Стрелки
        float sec = t.s + secFrac;
        float aH  = ((t.h % 12) + t.m / 60.0f) * 30.0f * DEG_TO_RAD;
        float aM  = (t.m + t.s / 60.0f) * 6.0f * DEG_TO_RAD;
        float aS  = sec * 6.0f * DEG_TO_RAD;

        drawHand(aH, 58,  5.0f, 12, C_WHITE);
        drawHand(aM, 88,  3.5f, 14, C_CYAN);
        drawHand(aS, 102, 1.2f, 22, C_RED);

        spr.fillCircle(120, 120, 6, C_WHITE);
        spr.fillCircle(120, 120, 3, C_RED);

        char buf[16];
        snprintf(buf, sizeof(buf), "%s %02d", WDAYS[t.wday], t.day);
        spr.setTextSize(1);
        spr.setTextColor(C_GREY);
        spr.drawString(buf, 120, 162);

        return true;
    }
};

Screen* clockAnalogScreen() {
    static ClockAnalogScreen s;
    return &s;
}
