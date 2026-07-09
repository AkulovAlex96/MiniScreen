#pragma once
#include "screen.h"

// ─── Менеджер экранов ─────────────────────────────────────────────────────────
// Реестр — явный массив в screens/registry.cpp. Менеджер знает, какие экраны
// включены (hubCfg.enabledCsv), какой активен, пейсит tick() по frameDelayMs()
// экрана и рисует оверлей с названием ~1.5с после переключения.

void smBegin();                        // активировать сохранённый/первый включённый экран
void smLoop();
void smNext();                         // следующий включённый (кнопки/авторотация)
bool smActivate(const char* id);       // false = нет такого id или экран выключен
Screen* smActive();

Screen* const* smAll(int& count);      // весь реестр (для веба)

// enabledCsv поменялся из веба: если активный экран выключили — уйти с него
void smEnabledChanged();
// Разослать onConfigChanged экрану по id ("hub" = всем)
void smNotifyConfigChanged(const char* id);
