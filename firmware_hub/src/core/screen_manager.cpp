#include "screen_manager.h"
#include "config.h"
#include "gfx.h"

// Реестр определён в screens/registry.cpp
extern Screen* const kScreens[];
extern const int kScreenCount;

static int      activeIdx  = -1;
static uint32_t switchMs   = 0;
static uint32_t lastTickMs = 0;

static const uint32_t OVERLAY_MS = 1500;

Screen* smActive() { return activeIdx >= 0 ? kScreens[activeIdx] : nullptr; }

Screen* const* smAll(int& count) { count = kScreenCount; return kScreens; }

static int findIdx(const char* id) {
    for (int i = 0; i < kScreenCount; i++)
        if (strcmp(kScreens[i]->id(), id) == 0) return i;
    return -1;
}

static void activateIdx(int idx) {
    if (idx == activeIdx) return;
    if (activeIdx >= 0) kScreens[activeIdx]->onExit();
    activeIdx = idx;
    switchMs = millis();
    lastTickMs = 0;   // первый tick нового экрана — сразу
    if (idx >= 0) {
        kScreens[idx]->onEnter();
        if (hubCfg.activeId != kScreens[idx]->id()) {
            hubCfg.activeId = kScreens[idx]->id();
            hubSave();
        }
        Serial.printf("Screen: %s\n", kScreens[idx]->title());
    }
}

bool smActivate(const char* id) {
    int idx = findIdx(id);
    if (idx < 0 || !screenEnabled(id)) return false;
    activateIdx(idx);
    return true;
}

static int firstEnabledIdx() {
    for (int i = 0; i < kScreenCount; i++)
        if (screenEnabled(kScreens[i]->id())) return i;
    return -1;
}

void smBegin() {
    if (hubCfg.activeId.length() && smActivate(hubCfg.activeId.c_str())) return;
    activateIdx(firstEnabledIdx());
}

void smNext() {
    if (kScreenCount == 0) return;
    int start = activeIdx < 0 ? 0 : activeIdx;
    for (int step = 1; step <= kScreenCount; step++) {
        int idx = (start + step) % kScreenCount;
        if (screenEnabled(kScreens[idx]->id())) {
            activateIdx(idx);
            return;
        }
    }
}

void smEnabledChanged() {
    Screen* s = smActive();
    if (s && screenEnabled(s->id())) return;
    int idx = firstEnabledIdx();
    if (idx >= 0) activateIdx(idx);
    // Все выключены — оставляем текущий: пустой экран хуже «лишнего»
}

void smNotifyConfigChanged(const char* id) {
    for (int i = 0; i < kScreenCount; i++)
        if (strcmp(id, "hub") == 0 || strcmp(kScreens[i]->id(), id) == 0)
            kScreens[i]->onConfigChanged();
}

// Название экрана — вкомпоновывается в spr ПЕРЕД pushSprite, поэтому уходит
// на дисплей одним куском с кадром и не моргает.
static void drawOverlay(Screen* s) {
    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextSize(1);
    spr.setTextDatum(lgfx::middle_center);
    int nameW = spr.textWidth(s->title());
    spr.fillRoundRect(120 - nameW / 2 - 8, 204, nameW + 16, 18, 8, C_DGREY);
    spr.setTextColor(C_WHITE);
    spr.drawString(s->title(), 120, 213);
}

void smLoop() {
    Screen* s = smActive();
    if (!s) return;
    uint32_t now = millis();

    if (lastTickMs == 0 || now - lastTickMs >= s->frameDelayMs()) {
        lastTickMs = now;
        if (s->tick(now)) {   // кадр обновлён — оверлей и единый пуш
            if (now - switchMs < OVERLAY_MS) drawOverlay(s);
            spr.pushSprite(0, 0);
        }
    }

    if (hubCfg.rotateSec > 0 && now - switchMs >= hubCfg.rotateSec * 1000UL) smNext();
}
