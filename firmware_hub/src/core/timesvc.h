#pragma once
#include <Arduino.h>

// ─── Время: NTP + календарные helpers ─────────────────────────────────────────
// Реальное время по сети (pool.ntp.org). Пока не синхронизировано — экраны
// часов/календаря честно показывают "no time", а не 1970 год.
// Часовой пояс — hubCfg.tzHours, после смены вызвать timeApplyTz().

struct DateTime { int year, mon, day, h, m, s, wday; };   // wday: 0=Mon..6=Sun

extern const char* const MONTHS[12];   // "Jan".."Dec"
extern const char* const WDAYS[7];     // "Mon".."Sun"

void timeLoop();          // конфигурирует SNTP при появлении сети, следит за синком
bool timeSynced();
void timeApplyTz();       // переконфигурить SNTP после смены hubCfg.tzHours

DateTime timeNow();
float timeSecFrac();      // дробная часть текущей секунды 0..1 (плавные стрелки)

bool isLeap(int y);
int  daysInMonth(int y, int m);
int  firstWeekdayOfMonth(int year, int mon);   // 0=Mon..6=Sun
