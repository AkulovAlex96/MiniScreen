#include <HTTPClient.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include "../core/screen.h"
#include "../core/gfx.h"
#include "../core/net.h"

// ─── Тикер криптовалюты (Binance public REST, без ключа) ──────────────────────
// Несколько независимых слотов одного класса (crypto1..crypto4), как у пяти
// виджетов в demos.cpp: каждый слот сам хранит пару и интервал свечи в своей
// записи Preferences (неймспейс = id слота), координаты дома тут не нужны.
//
// /ticker/24hr -> цена, % изменения, мин/макс, объём (раз в TICKER_POLL_MS).
// /klines?interval=.. -> цены закрытия последних KLINE_POINTS свечей для
// графика — интервал и есть та самая веб-настройка "время": он одновременно
// шаг свечи и (через KLINE_POINTS штук) глубина истории на графике.
//
// BTN1 short — рефреш прямо сейчас. Binance может быть недоступен из
// некоторых стран (гео-ограничения) — в этом случае экран покажет "no data"
// по HTTP-коду в серийном логе, замены на другую биржу пока нет.

namespace crypto {

struct Coin { const char* symbol; const char* label; const char* pair; uint16_t color; };
// Цвета — RGB565, осветлённые брендовые оттенки монет (гуще читаются на
// тёмном фоне экрана). rgb565()/gfx.h не constexpr, поэтому посчитаны заранее.
static const Coin COINS[] = {
    {"BTCUSDT",  "BTC",  "BTC/USDT",  0xF483},
    {"ETHUSDT",  "ETH",  "ETH/USDT",  0x8CBF},
    {"BNBUSDT",  "BNB",  "BNB/USDT",  0xF5C1},
    {"SOLUSDT",  "SOL",  "SOL/USDT",  0x57F8},
    {"XRPUSDT",  "XRP",  "XRP/USDT",  0x8D59},
    {"ADAUSDT",  "ADA",  "ADA/USDT",  0x4B7D},
    {"DOGEUSDT", "DOGE", "DOGE/USDT", 0xDDCA},
    {"TONUSDT",  "TON",  "TON/USDT",  0x4D9D},
    {"TRXUSDT",  "TRX",  "TRX/USDT",  0xFA6B},
    {"LTCUSDT",  "LTC",  "LTC/USDT",  0x957A},
    {"DOTUSDT",  "DOT",  "DOT/USDT",  0xFA75},
    {"LINKUSDT", "LINK", "LINK/USDT", 0x543F},
    {"AVAXUSDT", "AVAX", "AVAX/USDT", 0xFAEB},
    {"SHIBUSDT", "SHIB", "SHIB/USDT", 0xFBC3},
};
static const int COIN_COUNT = sizeof(COINS) / sizeof(COINS[0]);

struct Ival { const char* code; };
static const Ival INTERVALS[] = {
    {"1m"}, {"5m"}, {"15m"}, {"30m"},
    {"1h"}, {"2h"}, {"4h"}, {"8h"}, {"12h"}, {"1d"},
};
static const int INTERVAL_COUNT = sizeof(INTERVALS) / sizeof(INTERVALS[0]);
static const int DEFAULT_INTERVAL_IDX = 4;   // "1h"

static int findCoin(const String& sym) {
    for (int i = 0; i < COIN_COUNT; i++) if (sym == COINS[i].symbol) return i;
    return -1;
}
static int findInterval(const String& code) {
    for (int i = 0; i < INTERVAL_COUNT; i++) if (code == INTERVALS[i].code) return i;
    return -1;
}

static const int      KLINE_POINTS     = 48;
static const uint32_t TICKER_POLL_MS   = 45UL * 1000UL;
static const uint32_t KLINES_POLL_MS   = 5UL * 60UL * 1000UL;

// Разделитель тысяч (пробел) в целой части. Знак "-" в счёт группировки не
// идёт — на входе он либо отсутствует (цены всегда >=0), либо первым символом.
static void insertThousands(const char* in, char* out, size_t outSize) {
    int start = (in[0] == '-') ? 1 : 0;
    int o = 0;
    if (start) out[o++] = '-';
    const char* dot = strchr(in, '.');
    int intLen = (int)((dot ? dot : in + strlen(in)) - in);
    for (int i = start; i < intLen && o < (int)outSize - 1; i++) {
        int remaining = intLen - i;
        if (i > start && remaining % 3 == 0) out[o++] = ' ';
        out[o++] = in[i];
    }
    for (int i = intLen; in[i] && o < (int)outSize - 1; i++) out[o++] = in[i];
    out[o] = '\0';
}

static void fmtPrice(float v, char* out, size_t n) {
    float av = fabsf(v);
    int dec = av >= 100.0f ? 2 : av >= 1.0f ? 3 : av >= 0.01f ? 5 : 8;
    char raw[24];
    snprintf(raw, sizeof(raw), "%.*f", dec, v);
    insertThousands(raw, out, n);
}

static void fmtCompact(float v, char* out, size_t n) {
    float av = fabsf(v);
    if      (av >= 1e9f) snprintf(out, n, "%.1fB", v / 1e9f);
    else if (av >= 1e6f) snprintf(out, n, "%.1fM", v / 1e6f);
    else if (av >= 1e3f) snprintf(out, n, "%.1fK", v / 1e3f);
    else                  snprintf(out, n, "%.2f", v);
}

// Затемнённая версия цвета (для псевдо-градиента заливки под графиком) —
// масштабируем каналы прямо в 5/6/5, без похода через 8-битный RGB и обратно.
static uint16_t dimColor(uint16_t col, float k) {
    uint8_t r = (col >> 11) & 0x1F, g = (col >> 5) & 0x3F, b = col & 0x1F;
    r = (uint8_t)(r * k); g = (uint8_t)(g * k); b = (uint8_t)(b * k);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

} // namespace crypto

class CryptoScreen : public Screen {
    const char* slotId_;
    const char* slotTitle_;
    int defaultCoinIdx_;

    Preferences prefs;
    bool configLoaded = false;

    int coinIdx = 0;
    int intervalIdx = crypto::DEFAULT_INTERVAL_IDX;

    float lastPrice = 0, changePct = 0, changeAbs = 0, hi24 = 0, lo24 = 0, vol24 = 0;
    float closes[crypto::KLINE_POINTS];
    int   closesLen = 0;

    bool haveTicker = false, haveKlines = false;
    bool forceRefresh = true, needsRedraw = true;
    uint32_t lastTickerMs = 0, lastKlinesMs = 0;

    void loadConfig() {
        using namespace crypto;
        prefs.begin(slotId_, true);
        String sym = prefs.getString("sym", COINS[defaultCoinIdx_].symbol);
        String iv  = prefs.getString("intv", INTERVALS[DEFAULT_INTERVAL_IDX].code);
        prefs.end();
        int ci = findCoin(sym);
        coinIdx = ci >= 0 ? ci : defaultCoinIdx_;
        int ii = findInterval(iv);
        intervalIdx = ii >= 0 ? ii : DEFAULT_INTERVAL_IDX;
    }

    void saveConfig() {
        using namespace crypto;
        prefs.begin(slotId_, false);
        prefs.putString("sym", COINS[coinIdx].symbol);
        prefs.putString("intv", INTERVALS[intervalIdx].code);
        prefs.end();
    }

    bool fetchTicker() {
        using namespace crypto;
        String url = "https://api.binance.com/api/v3/ticker/24hr?symbol=";
        url += COINS[coinIdx].symbol;

        HTTPClient http;
        http.setTimeout(8000);
        http.begin(url);
        int code = http.GET();
        if (code != HTTP_CODE_OK) {
            Serial.printf("Binance ticker HTTP %d\n", code);
            http.end();
            return false;
        }
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getStream());
        http.end();
        if (err) { Serial.printf("ticker JSON error: %s\n", err.c_str()); return false; }

        lastPrice  = doc["lastPrice"].as<String>().toFloat();
        changePct  = doc["priceChangePercent"].as<String>().toFloat();
        changeAbs  = doc["priceChange"].as<String>().toFloat();
        hi24       = doc["highPrice"].as<String>().toFloat();
        lo24       = doc["lowPrice"].as<String>().toFloat();
        vol24      = doc["volume"].as<String>().toFloat();
        haveTicker = true;
        return true;
    }

    bool fetchKlines() {
        using namespace crypto;
        String url = "https://api.binance.com/api/v3/klines?symbol=";
        url += COINS[coinIdx].symbol;
        url += "&interval=";
        url += INTERVALS[intervalIdx].code;
        url += "&limit=";
        url += String(KLINE_POINTS);

        HTTPClient http;
        http.setTimeout(8000);
        http.begin(url);
        int code = http.GET();
        if (code != HTTP_CODE_OK) {
            Serial.printf("Binance klines HTTP %d\n", code);
            http.end();
            return false;
        }
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getStream());
        http.end();
        if (err) { Serial.printf("klines JSON error: %s\n", err.c_str()); return false; }
        if (!doc.is<JsonArray>()) { Serial.println("klines: unexpected payload"); return false; }

        closesLen = 0;
        for (JsonArray k : doc.as<JsonArray>()) {
            if (closesLen >= KLINE_POINTS) break;
            closes[closesLen++] = k[4].as<String>().toFloat();   // [4] = close price
        }
        haveKlines = closesLen > 1;
        return haveKlines;
    }

    void drawScreen() {
        using namespace crypto;
        const Coin& coin = COINS[coinIdx];
        bool gain = changePct >= 0.0f;
        uint16_t trendCol = gain ? C_GREEN : C_RED;

        spr.fillSprite(C_BLACK);

        // Пара + цветная точка монеты (не прижимаем к левому краю — обрежет круг)
        spr.setFont(&lgfx::fonts::Font0);
        spr.setTextSize(2);
        spr.setTextColor(C_WHITE);
        spr.setTextDatum(lgfx::middle_left);
        int pw = spr.textWidth(coin.pair);
        int textX = 120 - pw / 2 + 8;
        spr.drawString(coin.pair, textX, 26);
        spr.fillCircle(textX - 11, 26, 5, coin.color);

        // Цена — векторный шрифт (FreeSansBold), а не растровый Font4 x2:
        // при целочисленном масштабировании битмап-шрифтов получалась грубая
        // "лесенка". Кегль поменьше, чем раньше — освобождает место графику.
        // У дешёвых монет (SHIB и т.п.) строка длиннее — падаем на кегль
        // поменьше, если не влезает.
        char priceBuf[24];
        fmtPrice(lastPrice, priceBuf, sizeof(priceBuf));
        spr.setTextDatum(lgfx::middle_center);
        spr.setTextColor(C_WHITE);
        spr.setTextSize(1);
        spr.setFont(&lgfx::fonts::FreeSansBold18pt7b);
        if (spr.textWidth(priceBuf) > 190) spr.setFont(&lgfx::fonts::FreeSansBold12pt7b);
        spr.drawString(priceBuf, 120, 58);

        // Изменение за 24ч
        char absBuf[24], chBuf[40];
        fmtPrice(fabsf(changeAbs), absBuf, sizeof(absBuf));
        snprintf(chBuf, sizeof(chBuf), "%+.2f%%  %s%s", changePct, gain ? "+" : "-", absBuf);
        spr.setFont(&lgfx::fonts::FreeSansBold12pt7b);
        spr.setTextSize(1);
        spr.setTextColor(trendCol);
        spr.setTextDatum(lgfx::middle_center);
        spr.drawString(chBuf, 120, 86);

        // Спарклайн из цен закрытия свечей
        if (haveKlines && closesLen > 1) {
            const int x0 = 26, x1 = 214, yTop = 102, yBase = 188;
            float mn = closes[0], mx = closes[0];
            for (int i = 1; i < closesLen; i++) { mn = min(mn, closes[i]); mx = max(mx, closes[i]); }
            float span = (mx - mn) < 1e-6f ? 1.0f : (mx - mn);
            uint16_t fillCol = dimColor(trendCol, 0.30f);

            // Горизонтальные полосы-ориентиры — рисуем ДО заливки: ниже линии
            // цены их перекроет fillCol, выше останутся видны как лёгкая сетка.
            for (int g = 1; g <= 3; g++) {
                int gy = yTop + (yBase - yTop) * g / 4;
                spr.drawFastHLine(x0, gy, x1 - x0 + 1, C_DGREY);
            }

            int prevY = yBase;
            for (int x = x0; x <= x1; x++) {
                float idxF = (float)(x - x0) / (float)(x1 - x0) * (closesLen - 1);
                int i0 = (int)idxF;
                if (i0 >= closesLen - 1) i0 = closesLen - 2;
                float frac = idxF - i0;
                float v = closes[i0] * (1.0f - frac) + closes[i0 + 1] * frac;
                int y = yBase - (int)((v - mn) / span * (yBase - yTop));
                spr.drawFastVLine(x, y, yBase - y + 1, fillCol);
                if (x > x0) spr.drawLine(x - 1, prevY, x, y, trendCol);
                prevY = y;
            }
            spr.fillCircle(x1, prevY, 3, trendCol);
        } else {
            spr.setFont(&lgfx::fonts::Font0);
            spr.setTextSize(1);
            spr.setTextColor(C_GREY);
            spr.setTextDatum(lgfx::middle_center);
            spr.drawString("no chart data", 120, 145);
        }

        // Мин/Макс/Объём за 24ч — две короткие строки (нижняя хорда круга уже)
        char loBuf[24], hiBuf[24], volBuf[16], statBuf[48];
        fmtPrice(lo24, loBuf, sizeof(loBuf));
        fmtPrice(hi24, hiBuf, sizeof(hiBuf));
        fmtCompact(vol24, volBuf, sizeof(volBuf));

        spr.setFont(&lgfx::fonts::Font0);
        spr.setTextSize(1);
        spr.setTextColor(C_GREY);
        spr.setTextDatum(lgfx::middle_center);
        snprintf(statBuf, sizeof(statBuf), "MIN %s   MAX %s", loBuf, hiBuf);
        spr.drawString(statBuf, 120, 202);
        snprintf(statBuf, sizeof(statBuf), "VOL %s %s", volBuf, coin.label);
        spr.drawString(statBuf, 120, 219);
    }

public:
    CryptoScreen(const char* slotId, const char* slotTitle, int defaultCoinIdx)
        : slotId_(slotId), slotTitle_(slotTitle), defaultCoinIdx_(defaultCoinIdx) {}

    const char* id() const override    { return slotId_; }
    const char* title() const override { return slotTitle_; }

    void onEnter() override {
        if (!configLoaded) { loadConfig(); configLoaded = true; }
        needsRedraw = true;   // экран мог давно не обновляться, пока был неактивен
    }

    bool tick(uint32_t nowMs) override {
        using namespace crypto;
        if (!netUp()) {
            statusScreen(COINS[coinIdx].pair, "no WiFi", C_RED);
            return true;
        }

        if (forceRefresh || nowMs - lastTickerMs >= TICKER_POLL_MS) {
            lastTickerMs = nowMs;
            if (fetchTicker()) needsRedraw = true;
        }
        if (forceRefresh || nowMs - lastKlinesMs >= KLINES_POLL_MS) {
            lastKlinesMs = nowMs;
            if (fetchKlines()) needsRedraw = true;
        }
        forceRefresh = false;

        if (!haveTicker) {
            statusScreen(COINS[coinIdx].pair, "loading...", C_CYAN);
            return true;
        }
        if (!needsRedraw) return false;
        needsRedraw = false;
        drawScreen();
        return true;
    }

    uint32_t frameDelayMs() const override { return 500; }

    void onButton(BtnEvent ev) override {
        if (ev == EV_BTN1_SHORT) forceRefresh = true;
    }

    void getConfig(JsonObject out) override {
        using namespace crypto;
        out["symbol"] = COINS[coinIdx].symbol;
        JsonArray symOpts = out["symbolOptions"].to<JsonArray>();
        for (int i = 0; i < COIN_COUNT; i++) symOpts.add(COINS[i].symbol);

        out["interval"] = INTERVALS[intervalIdx].code;
        JsonArray ivOpts = out["intervalOptions"].to<JsonArray>();
        for (int i = 0; i < INTERVAL_COUNT; i++) ivOpts.add(INTERVALS[i].code);
    }

    void applyConfig(JsonObjectConst in) override {
        using namespace crypto;
        bool symChanged = false, ivChanged = false;
        if (!in["symbol"].isNull()) {
            int idx = findCoin(in["symbol"].as<const char*>());
            if (idx >= 0 && idx != coinIdx) { coinIdx = idx; symChanged = true; }
        }
        if (!in["interval"].isNull()) {
            int idx = findInterval(in["interval"].as<const char*>());
            if (idx >= 0 && idx != intervalIdx) { intervalIdx = idx; ivChanged = true; }
        }
        if (symChanged || ivChanged) {
            saveConfig();
            if (symChanged) haveTicker = false;
            haveKlines = false;   // символ или интервал сменился — свечи всегда переспрашиваем
            forceRefresh = true;
            needsRedraw = true;
        }
    }
};

Screen* cryptoScreen1() { static CryptoScreen s("crypto1", "Crypto 1", 0); return &s; }   // BTC
Screen* cryptoScreen2() { static CryptoScreen s("crypto2", "Crypto 2", 1); return &s; }   // ETH
Screen* cryptoScreen3() { static CryptoScreen s("crypto3", "Crypto 3", 2); return &s; }   // BNB
Screen* cryptoScreen4() { static CryptoScreen s("crypto4", "Crypto 4", 3); return &s; }   // SOL
