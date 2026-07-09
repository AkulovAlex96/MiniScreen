#include "buttons.h"

static const uint32_t LONG_PRESS_MS = 600;

struct BtnState {
    uint8_t  pin;
    bool     wasDown;
    uint32_t downMs;
    bool     longFired;
};

static BtnState btn1 = {BTN1_PIN, false, 0, false};
static BtnState btn2 = {BTN2_PIN, false, 0, false};

void buttonsInit() {
    pinMode(BTN1_PIN, INPUT_PULLUP);
    pinMode(BTN2_PIN, INPUT_PULLUP);
    pinMode(TOUCH_PIN, INPUT_PULLDOWN);
}

// Детектор короткого/долгого без блокировок (из firmware_gif_sd)
static BtnEvent pollBtn(BtnState& b, BtnEvent shortEv, BtnEvent longEv) {
    bool isDown = !digitalRead(b.pin);   // активный уровень — LOW
    BtnEvent ev = EV_NONE;
    if (isDown && !b.wasDown) {
        b.downMs = millis();
        b.longFired = false;
    } else if (isDown && b.wasDown && !b.longFired && millis() - b.downMs > LONG_PRESS_MS) {
        b.longFired = true;
        ev = longEv;
    } else if (!isDown && b.wasDown && !b.longFired) {
        ev = shortEv;
    }
    b.wasDown = isDown;
    return ev;
}

// TTP223 сам активно драйвит пин, HIGH = касание; edge-детект по фронту
static bool touchPressed() {
    static bool lastState = false;
    static uint32_t lastChangeMs = 0;
    bool state = digitalRead(TOUCH_PIN) == HIGH;
    uint32_t now = millis();
    if (state != lastState && now - lastChangeMs > 50) {
        lastChangeMs = now;
        lastState = state;
        return state;
    }
    return false;
}

BtnEvent buttonsPoll() {
    BtnEvent ev = pollBtn(btn1, EV_BTN1_SHORT, EV_BTN1_LONG);
    if (ev != EV_NONE) return ev;
    ev = pollBtn(btn2, EV_BTN2_SHORT, EV_BTN2_LONG);
    if (ev != EV_NONE) return ev;
    if (touchPressed()) return EV_TOUCH;
    return EV_NONE;
}
