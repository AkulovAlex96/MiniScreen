#include "timesvc.h"
#include <time.h>
#include <sys/time.h>
#include "net.h"
#include "config.h"

const char* const MONTHS[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
const char* const WDAYS[7]   = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};

static bool synced  = false;
static bool cfgSent = false;

void timeApplyTz() {
    cfgSent = false;   // timeLoop() перезапустит configTime с новым смещением
}

void timeLoop() {
    if (!netUp()) return;
    if (!cfgSent) {
        configTime((long)(hubCfg.tzHours * 3600.0f), 0, "pool.ntp.org", "time.google.com");
        cfgSent = true;
    }
    if (!synced) {
        // Не getLocalTime(..., timeout): он блокирует. Просто проверяем, что
        // SNTP уже подвинул системные часы из 1970 года.
        if (time(nullptr) > 1700000000) {
            synced = true;
            DateTime t = timeNow();
            Serial.printf("Время синхронизировано: %04d-%02d-%02d %02d:%02d\n",
                          t.year, t.mon, t.day, t.h, t.m);
        }
    }
}

bool timeSynced() { return synced; }

DateTime timeNow() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    DateTime dt;
    dt.year = tmv.tm_year + 1900;
    dt.mon  = tmv.tm_mon + 1;
    dt.day  = tmv.tm_mday;
    dt.h    = tmv.tm_hour;
    dt.m    = tmv.tm_min;
    dt.s    = tmv.tm_sec;
    dt.wday = (tmv.tm_wday + 6) % 7;   // tm_wday: 0=Sun..6=Sat -> 0=Mon..6=Sun
    return dt;
}

float timeSecFrac() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_usec / 1e6f;
}

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int daysInMonth(int y, int m) {
    static const int d[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return (m == 2 && isLeap(y)) ? 29 : d[m - 1];
}

// Через mktime/localtime (а не Zeller) — стандартная либа уже корректно знает
// про високосные года и переносы.
int firstWeekdayOfMonth(int year, int mon) {
    struct tm tmv = {};
    tmv.tm_year = year - 1900;
    tmv.tm_mon  = mon - 1;
    tmv.tm_mday = 1;
    tmv.tm_hour = 12;   // подальше от полуночи, чтобы не зацепить сдвиг DST
    time_t t = mktime(&tmv);
    struct tm out;
    localtime_r(&t, &out);
    return (out.tm_wday + 6) % 7;
}
