#include <Arduino.h>
#include <WiFi.h>
#include "core/gfx.h"
#include "core/net.h"
#include "core/config.h"
#include "core/buttons.h"
#include "core/timesvc.h"
#include "core/decoders.h"
#include "core/storage.h"
#include "core/screen_manager.h"
#include "core/webui.h"

// ═══ MiniScreen Hub ════════════════════════════════════════════════════════════
// Объединённая прошивка: все экраны — модули (src/screens/*), ядро — склейка.
// Архитектура и правила добавления экранов: ARCHITECTURE.md.
//
// BTN1 (GPIO1)  — short/long: отдаётся активному экрану (радар: рефреш; GIF: след/пред)
// BTN2 (GPIO2)  — short: яркость по кругу; long: следующий экран
// TTP223 (GPIO15) — следующий экран

void setup() {
    Serial.begin(115200);
    Serial.println("MiniScreen Hub");
    Serial.printf("PSRAM: %u bytes, heap: %u\n", ESP.getPsramSize(), ESP.getFreeHeap());

    randomSeed(esp_random());
    buttonsInit();
    hubLoad();
    gfx::init();
    gfx::applyBrightness();
    decodersInit();
    sdInitBus();

    if (netConnect()) {
        statusScreen("Web UI", WiFi.localIP().toString().c_str(), C_CYAN);
        spr.pushSprite(0, 0);   // мы вне tick() экрана — пушим сами
        delay(1500);
        webuiStart();
    }

    smBegin();
}

void loop() {
    switch (buttonsPoll()) {
        case EV_BTN2_SHORT: gfx::cycleBrightness(); break;
        case EV_BTN2_LONG:
        case EV_TOUCH:      smNext(); break;
        case EV_BTN1_SHORT: if (smActive()) smActive()->onButton(EV_BTN1_SHORT); break;
        case EV_BTN1_LONG:  if (smActive()) smActive()->onButton(EV_BTN1_LONG);  break;
        default: break;
    }

    netLoop();
    timeLoop();
    if (netUp()) webuiStart();   // если Wi-Fi поднялся позже старта
    webuiLoop();
    smLoop();

    delay(2);   // pacing кадров — внутри smLoop по frameDelayMs() экрана
}
