#include "../core/screen.h"
#include "../core/gfx.h"
#include "../core/net.h"
#include "../core/timesvc.h"

// ─── Цифровые часы (из firmware_widgets, время — NTP) ─────────────────────────

class ClockDigitalScreen : public Screen {
public:
    const char* id() const override    { return "clock_digital"; }
    const char* title() const override { return "Digital clock"; }

    bool tick(uint32_t) override {
        if (!timeSynced()) {
            statusScreen("no time", netUp() ? "syncing..." : "no WiFi", C_RED);
            return true;
        }
        DateTime t = timeNow();
        float secFrac = timeSecFrac();

        spr.fillSprite(C_BLACK);

        // Секундная дуга по краю
        arcSeg(119, 115, -90.0f, -90.0f + (t.s + secFrac) * 6.0f, C_ACCENT);

        // HH : MM крупным 7-сегментным шрифтом
        char hh[3], mm[3];
        snprintf(hh, sizeof(hh), "%02d", t.h);
        snprintf(mm, sizeof(mm), "%02d", t.m);

        spr.setFont(&lgfx::fonts::Font7);
        spr.setTextSize(1);
        spr.setTextColor(C_WHITE);

        int wc = spr.textWidth(":");
        spr.setTextDatum(lgfx::middle_right);
        spr.drawString(hh, 120 - wc / 2 - 2, 100);
        spr.setTextDatum(lgfx::middle_left);
        spr.drawString(mm, 120 + wc / 2 + 2, 100);

        // Мигающее двоеточие
        if (secFrac < 0.5f) {
            spr.setTextDatum(lgfx::middle_center);
            spr.drawString(":", 120, 96);
        }

        spr.setFont(&lgfx::fonts::Font0);
        spr.setTextDatum(lgfx::middle_center);
        spr.setTextSize(2);
        spr.setTextColor(C_CYAN);
        char buf[24];
        snprintf(buf, sizeof(buf), "%02d", t.s);
        spr.drawString(buf, 120, 148);

        spr.setTextSize(1);
        spr.setTextColor(C_GREY);
        snprintf(buf, sizeof(buf), "%s %02d %s %d", WDAYS[t.wday], t.day, MONTHS[t.mon - 1], t.year);
        spr.drawString(buf, 120, 176);

        return true;
    }
};

Screen* clockDigitalScreen() {
    static ClockDigitalScreen s;
    return &s;
}
