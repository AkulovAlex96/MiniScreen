#pragma once
#include <Arduino.h>

// ─── Глобальный конфиг хаба (NVS namespace "hub") ─────────────────────────────
// Настройки, общие для всех экранов. Свои настройки каждый экран хранит в
// собственном NVS-неймспейсе (см. например screens/radar.cpp).

struct HubConfig {
    double   homeLat;        // координаты дома — общие для radar/clouds/rain/map
    double   homeLon;
    float    tzHours;        // часовой пояс, часов от UTC (можно дробный)
    int      brightnessIdx;  // индекс в gfx::BRIGHTNESS_LEVELS
    uint32_t rotateSec;      // авторотация экранов, сек (0 = выключена)
    String   enabledCsv;     // CSV из id включённых экранов, "*" = все
    String   activeId;       // id активного экрана
};

extern HubConfig hubCfg;

void hubLoad();
void hubSave();

// Входит ли id в enabledCsv ("*" = да для всех)
bool screenEnabled(const char* id);
