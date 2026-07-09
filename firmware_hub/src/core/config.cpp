#include "config.h"
#include <Preferences.h>
#include "../secrets.h"

HubConfig hubCfg;
static Preferences prefs;

static void clampHub() {
    if (hubCfg.homeLat < -85.0)  hubCfg.homeLat = -85.0;
    if (hubCfg.homeLat > 85.0)   hubCfg.homeLat = 85.0;
    if (hubCfg.homeLon < -180.0) hubCfg.homeLon = -180.0;
    if (hubCfg.homeLon > 180.0)  hubCfg.homeLon = 180.0;
    if (hubCfg.tzHours < -12.0f) hubCfg.tzHours = -12.0f;
    if (hubCfg.tzHours > 14.0f)  hubCfg.tzHours = 14.0f;
    if (hubCfg.rotateSec != 0 && hubCfg.rotateSec < 5) hubCfg.rotateSec = 5;
}

void hubLoad() {
    prefs.begin("hub", true);
    hubCfg.homeLat       = prefs.getDouble("lat", HOME_LAT);
    hubCfg.homeLon       = prefs.getDouble("lon", HOME_LON);
    hubCfg.tzHours       = prefs.getFloat("tz", 5.0f);
    hubCfg.brightnessIdx = prefs.getInt("bri", 0);
    hubCfg.rotateSec     = prefs.getUInt("rot", 0);
    hubCfg.enabledCsv    = prefs.getString("en", "*");
    hubCfg.activeId      = prefs.getString("act", "");
    prefs.end();
    clampHub();
}

void hubSave() {
    clampHub();
    prefs.begin("hub", false);
    prefs.putDouble("lat", hubCfg.homeLat);
    prefs.putDouble("lon", hubCfg.homeLon);
    prefs.putFloat("tz", hubCfg.tzHours);
    prefs.putInt("bri", hubCfg.brightnessIdx);
    prefs.putUInt("rot", hubCfg.rotateSec);
    prefs.putString("en", hubCfg.enabledCsv);
    prefs.putString("act", hubCfg.activeId);
    prefs.end();
}

bool screenEnabled(const char* id) {
    if (hubCfg.enabledCsv == "*") return true;
    // Ищем id как целый CSV-токен, не подстроку (иначе "gif" совпал бы с "gifx")
    int start = 0;
    while (start < (int)hubCfg.enabledCsv.length()) {
        int comma = hubCfg.enabledCsv.indexOf(',', start);
        if (comma < 0) comma = hubCfg.enabledCsv.length();
        if (hubCfg.enabledCsv.substring(start, comma) == id) return true;
        start = comma + 1;
    }
    return false;
}
