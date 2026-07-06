#include <Arduino.h>
#include <LovyanGFX.hpp>

// ═══ MiniScreen widgets demo ══════════════════════════════════════════════════
// Рендер в полнокадровый спрайт (240x240x16bit = 115 КБ) → pushSprite одним
// блоком. Без мерцания, ~25-30 fps.
//
// ВАЖНО: цвета в LovyanGFX — тип определяет формат! uint16_t=RGB565, uint32_t=RGB888.
//
// BTN1 (GPIO1) — следующий виджет
// BTN2 (GPIO2) — подсветка вкл/выкл

// ─── LGFX: GC9A01 240x240 ────────────────────────────────────────────────────

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_GC9A01 _panel;
    lgfx::Bus_SPI      _bus;
    lgfx::Light_PWM    _backlight;
public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            // 40 МГц ок: «глюки цвета» на 40 МГц оказались багом типов, не SPI.
            // Если появятся артефакты на брэдборде — снизить до 20 МГц.
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = 11;
            cfg.pin_mosi    = 12;
            cfg.pin_miso    = -1;
            cfg.pin_dc      = 13;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs       = 10;
            cfg.pin_rst      = 14;
            cfg.pin_busy     = -1;
            cfg.panel_width  = 240;
            cfg.panel_height = 240;
            cfg.readable     = false;
            cfg.invert       = true;
            cfg.rgb_order    = false;
            cfg.dlen_16bit   = false;
            cfg.bus_shared   = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _backlight.config();
            cfg.pin_bl      = 21;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _backlight.config(cfg);
            _panel.setLight(&_backlight);
        }
        setPanel(&_panel);
    }
};

static LGFX tft;
static LGFX_Sprite spr(&tft);   // полнокадровый буфер

#define BTN1_PIN 1
#define BTN2_PIN 2

// ─── Цвета RGB565 (строго uint16_t!) ─────────────────────────────────────────

static const uint16_t C_BLACK  = 0x0000;
static const uint16_t C_WHITE  = 0xFFFF;
static const uint16_t C_RED    = 0xF800;
static const uint16_t C_GREEN  = 0x07E0;
static const uint16_t C_CYAN   = 0x07FF;
static const uint16_t C_YELLOW = 0xFFE0;
static const uint16_t C_GREY   = 0x7BEF;
static const uint16_t C_DGREY  = 0x39E7;
static const uint16_t C_ACCENT = 0x051F;  // ярко-голубой

static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint16_t hsv565(uint8_t h, uint8_t s, uint8_t v) {
    uint8_t r, g, b;
    if (s == 0) { r = g = b = v; }
    else {
        uint8_t region    = h / 43;
        uint8_t remainder = (h - region * 43) * 6;
        uint8_t p = (v * (255 - s)) >> 8;
        uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
        uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
        switch (region) {
            case 0: r=v; g=t; b=p; break;
            case 1: r=q; g=v; b=p; break;
            case 2: r=p; g=v; b=t; break;
            case 3: r=p; g=q; b=v; break;
            case 4: r=t; g=p; b=v; break;
            default: r=v; g=p; b=q; break;
        }
    }
    return rgb(r, g, b);
}

// ─── Часы: старт от времени компиляции + millis ──────────────────────────────
// (потом заменим на NTP по Wi-Fi)

static const char* MONTHS[] = {"Jan","Feb","Mar","Apr","May","Jun",
                               "Jul","Aug","Sep","Oct","Nov","Dec"};
static const char* WDAYS[]  = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};

struct DateTime { int year, mon, day, h, m, s, ms, wday; };  // wday: 0=Mon

static int baseSecOfDay;
static int baseYear, baseMon, baseDay;

static bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int daysInMonth(int y, int m) {
    static const int d[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return (m == 2 && isLeap(y)) ? 29 : d[m - 1];
}

// Zeller → 0=Mon..6=Sun
static int dayOfWeek(int y, int m, int d) {
    if (m < 3) { m += 12; y--; }
    int K = y % 100, J = y / 100;
    int h = (d + 13 * (m + 1) / 5 + K + K / 4 + J / 4 + 5 * J) % 7; // 0=Sat
    return (h + 5) % 7;
}

static void initClock() {
    // __TIME__ = "12:34:56", __DATE__ = "Jul  5 2026"
    int hh = 0, mm = 0, ss = 0;
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    baseSecOfDay = hh * 3600 + mm * 60 + ss;

    char monStr[4] = {0};
    sscanf(__DATE__, "%3s %d %d", monStr, &baseDay, &baseYear);
    baseMon = 1;
    for (int i = 0; i < 12; i++)
        if (strncmp(monStr, MONTHS[i], 3) == 0) { baseMon = i + 1; break; }
}

static DateTime now() {
    uint32_t ms = millis();
    long total = baseSecOfDay + ms / 1000;

    DateTime t;
    t.ms   = ms % 1000;
    t.year = baseYear; t.mon = baseMon; t.day = baseDay;

    long days = total / 86400;
    total %= 86400;
    while (days--) {
        if (++t.day > daysInMonth(t.year, t.mon)) {
            t.day = 1;
            if (++t.mon > 12) { t.mon = 1; t.year++; }
        }
    }
    t.h = total / 3600; t.m = (total / 60) % 60; t.s = total % 60;
    t.wday = dayOfWeek(t.year, t.mon, t.day);
    return t;
}

// ─── Кнопки ──────────────────────────────────────────────────────────────────

static bool btnPressed(int pin) {
    if (!digitalRead(pin)) { delay(20); while (!digitalRead(pin)); return true; }
    return false;
}

// ─── Общие помощники отрисовки ───────────────────────────────────────────────

// Дуга с поддержкой перехода через 360°
static void arcSeg(int r0, int r1, float a0, float a1, uint16_t col) {
    if (a1 > 360.0f) {
        spr.fillArc(120, 120, r0, r1, a0, 360.0f, col);
        spr.fillArc(120, 120, r0, r1, 0.0f, a1 - 360.0f, col);
    } else {
        spr.fillArc(120, 120, r0, r1, a0, a1, col);
    }
}

// Толстая стрелка часов (треугольник)
static void drawHand(float angRad, float len, float halfW, float tail, uint16_t col) {
    float dx = sinf(angRad), dy = -cosf(angRad);
    float px = -dy, py = dx;
    float tipX = 120 + dx * len,  tipY = 120 + dy * len;
    float bX   = 120 - dx * tail, bY   = 120 - dy * tail;
    spr.fillTriangle((int)tipX, (int)tipY,
                     (int)(bX + px * halfW), (int)(bY + py * halfW),
                     (int)(bX - px * halfW), (int)(bY - py * halfW), col);
}

// ═══ Виджет 0: Аналоговые часы ════════════════════════════════════════════════

static void wAnalogClock() {
    DateTime t = now();
    spr.fillSprite(C_BLACK);

    // Циферблат
    spr.fillArc(120, 120, 119, 115, 0.0f, 360.0f, C_DGREY);

    for (int i = 0; i < 60; i++) {
        float a = i * 6.0f * DEG_TO_RAD;
        float dx = sinf(a), dy = -cosf(a);
        int r1 = (i % 5 == 0) ? 100 : 108;
        uint16_t col = (i % 15 == 0) ? C_WHITE : C_GREY;
        spr.drawLine(120 + dx * r1, 120 + dy * r1,
                     120 + dx * 113, 120 + dy * 113, col);
    }

    // Цифры 12 / 3 / 6 / 9
    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextSize(2);
    spr.setTextColor(C_WHITE);
    spr.setTextDatum(lgfx::middle_center);
    spr.drawString("12", 120, 34);
    spr.drawString("3",  206, 120);
    spr.drawString("6",  120, 206);
    spr.drawString("9",   34, 120);

    // Стрелки
    float sec = t.s + t.ms / 1000.0f;
    float aH  = ((t.h % 12) + t.m / 60.0f) * 30.0f * DEG_TO_RAD;
    float aM  = (t.m + t.s / 60.0f) * 6.0f * DEG_TO_RAD;
    float aS  = sec * 6.0f * DEG_TO_RAD;

    drawHand(aH, 58,  5.0f, 12, C_WHITE);
    drawHand(aM, 88,  3.5f, 14, C_CYAN);
    drawHand(aS, 102, 1.2f, 22, C_RED);

    spr.fillCircle(120, 120, 6, C_WHITE);
    spr.fillCircle(120, 120, 3, C_RED);

    // Дата в нижней части циферблата
    char buf[16];
    snprintf(buf, sizeof(buf), "%s %02d", WDAYS[t.wday], t.day);
    spr.setTextSize(1);
    spr.setTextColor(C_GREY);
    spr.drawString(buf, 120, 162);
}

// ═══ Виджет 1: Цифровые часы ══════════════════════════════════════════════════

static void wDigitalClock() {
    DateTime t = now();
    spr.fillSprite(C_BLACK);

    // Секундная дуга по краю
    arcSeg(119, 115, -90.0f, -90.0f + (t.s + t.ms / 1000.0f) * 6.0f, C_ACCENT);

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
    if (t.ms < 500) {
        spr.setTextDatum(lgfx::middle_center);
        spr.drawString(":", 120, 96);
    }

    // Секунды и дата
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
}

// ═══ Виджет 2: Календарь ══════════════════════════════════════════════════════

static void wCalendar() {
    DateTime t = now();
    spr.fillSprite(C_BLACK);

    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextDatum(lgfx::middle_center);

    // Заголовок
    char buf[24];
    snprintf(buf, sizeof(buf), "%s %d", MONTHS[t.mon - 1], t.year);
    spr.setTextSize(2);
    spr.setTextColor(C_ACCENT);
    spr.drawString(buf, 120, 52);

    // Сетка: 7 столбцов x до 6 строк
    const int cellW = 26, cellH = 19;
    const int gridX = 120 - cellW * 7 / 2;   // 29
    const int gridY = 72;

    // Шапка дней недели
    static const char* WD = "MoTuWeThFrSaSu";
    spr.setTextSize(1);
    for (int i = 0; i < 7; i++) {
        char wd[3] = { WD[i * 2], WD[i * 2 + 1], 0 };
        spr.setTextColor(i >= 5 ? C_RED : C_GREY);
        spr.drawString(wd, gridX + i * cellW + cellW / 2, gridY + 6);
    }

    // Числа
    int firstCol = dayOfWeek(t.year, t.mon, 1);
    int dim      = daysInMonth(t.year, t.mon);
    int col = firstCol, row = 0;

    for (int d = 1; d <= dim; d++) {
        int cx = gridX + col * cellW + cellW / 2;
        int cy = gridY + 18 + row * cellH + cellH / 2;

        if (d == t.day) {
            spr.fillRoundRect(cx - 11, cy - 8, 22, 17, 5, C_ACCENT);
            spr.setTextColor(C_BLACK);
        } else {
            spr.setTextColor(col >= 5 ? C_RED : C_WHITE);
        }
        snprintf(buf, sizeof(buf), "%d", d);
        spr.drawString(buf, cx, cy);

        if (++col > 6) { col = 0; row++; }
    }
}

// ═══ Виджет 3: Круговой загрузчик ═════════════════════════════════════════════

static void wRingLoader() {
    spr.fillSprite(C_BLACK);

    // 0→100% по синусу, цикл ~6 сек
    float phase = millis() / 3000.0f * PI;
    float p = (1.0f - cosf(phase)) * 50.0f;   // 0..100

    // Фоновое кольцо + заполнение
    spr.fillArc(120, 120, 112, 100, 0.0f, 360.0f, C_DGREY);
    uint16_t col = hsv565((uint8_t)(p * 0.85f), 240, 255);  // красный → зелёный
    arcSeg(112, 100, -90.0f, -90.0f + p * 3.6f, col);

    // Точка-голова на конце дуги
    float aHead = (-90.0f + p * 3.6f) * DEG_TO_RAD;
    spr.fillCircle(120 + cosf(aHead) * 106, 120 + sinf(aHead) * 106, 7, C_WHITE);

    // Процент в центре
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)p);
    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextDatum(lgfx::middle_center);
    spr.setTextSize(4);
    spr.setTextColor(C_WHITE);
    spr.drawString(buf, 120, 112);

    spr.setTextSize(1);
    spr.setTextColor(C_GREY);
    spr.drawString(p > 99.0f ? "complete" : "loading...", 120, 152);
}

// ═══ Виджет 4: Плоский прогресс-бар ═══════════════════════════════════════════

static void wProgressBar() {
    spr.fillSprite(C_BLACK);

    float phase = millis() / 3000.0f * PI;
    float p = (1.0f - cosf(phase)) * 50.0f;

    const int bx = 45, by = 112, bw = 150, bh = 18;
    int fillW = (int)(bw * p / 100.0f);

    // Рамка и заполнение
    spr.drawRoundRect(bx - 2, by - 2, bw + 4, bh + 4, 9, C_GREY);
    if (fillW > 4)
        spr.fillRoundRect(bx, by, fillW, bh, 7, hsv565((uint8_t)(p * 0.85f), 240, 255));

    // Бегущий блик по заполненной части
    int shineW = 18;
    int shineX = bx + (int)(fmodf(millis() / 12.0f, (float)(bw + shineW))) - shineW;
    int sLeft  = max(shineX, bx);
    int sRight = min(shineX + shineW, bx + fillW);
    if (sRight > sLeft)
        spr.fillRect(sLeft, by + 3, sRight - sLeft, bh - 6, rgb(255, 255, 255));

    // Процент
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)p);
    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextDatum(lgfx::middle_center);
    spr.setTextSize(3);
    spr.setTextColor(C_WHITE);
    spr.drawString(buf, 120, 82);

    // Анимированные точки
    int dots = (millis() / 400) % 4;
    char dbuf[16] = "Downloading";
    for (int i = 0; i < dots; i++) strcat(dbuf, ".");
    spr.setTextSize(1);
    spr.setTextColor(C_GREY);
    spr.drawString(dbuf, 120, 156);
}

// ═══ Виджет 5: Стрелочный индикатор (gauge) ═══════════════════════════════════

static void wGauge() {
    spr.fillSprite(C_BLACK);

    // Значение — плавный «случайный» дрейф
    float tt = millis() / 1000.0f;
    float v = 50.0f + 35.0f * sinf(tt * 0.9f) + 15.0f * sinf(tt * 2.3f);
    v = constrain(v, 0.0f, 100.0f);

    // Шкала: 270°, от 135° до 405°
    const float A0 = 135.0f, SPAN = 270.0f;

    arcSeg(112, 102, A0, A0 + SPAN, C_DGREY);
    // Заполнение зелёный→красный
    arcSeg(112, 102, A0, A0 + SPAN * v / 100.0f,
           hsv565((uint8_t)(85 - v * 0.85f), 240, 255));

    // Риски каждые 10 единиц
    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextSize(1);
    spr.setTextDatum(lgfx::middle_center);
    for (int i = 0; i <= 10; i++) {
        float a = (A0 + SPAN * i / 10.0f) * DEG_TO_RAD;
        float dx = cosf(a), dy = sinf(a);
        spr.drawLine(120 + dx * 98, 120 + dy * 98, 120 + dx * 90, 120 + dy * 90,
                     (i % 5 == 0) ? C_WHITE : C_GREY);
        if (i % 5 == 0) {
            char lbl[4];
            snprintf(lbl, sizeof(lbl), "%d", i * 10);
            spr.setTextColor(C_GREY);
            spr.drawString(lbl, 120 + dx * 78, 120 + dy * 78);
        }
    }

    // Стрелка
    float aN = (A0 + SPAN * v / 100.0f) * DEG_TO_RAD;
    // drawHand использует угол от 12 часов — пересчёт: rad от +x → от вертикали
    drawHand(aN + PI / 2.0f, 84, 3.0f, 16, C_RED);
    spr.fillCircle(120, 120, 7, C_WHITE);
    spr.fillCircle(120, 120, 4, C_RED);

    // Значение
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", (int)v);
    spr.setTextSize(3);
    spr.setTextColor(C_WHITE);
    spr.drawString(buf, 120, 172);
    spr.setTextSize(1);
    spr.setTextColor(C_GREY);
    spr.drawString("km/h", 120, 196);
}

// ═══ Виджет 6: Матрица ════════════════════════════════════════════════════════
// Стейт-фул: спрайт не очищается между кадрами

static const int MTX_COLS = 20, MTX_ROWS = 15;   // ячейка 12x16 (шрифт 6x8 x2)
static int8_t  mtxHead[MTX_COLS];
static int8_t  mtxLen[MTX_COLS];
static uint8_t mtxDiv[MTX_COLS];   // делитель скорости
static uint8_t mtxCnt[MTX_COLS];

static void mtxResetCol(int c, bool randomY) {
    mtxHead[c] = randomY ? random(-MTX_ROWS, MTX_ROWS) : -random(1, 8);
    mtxLen[c]  = random(4, 12);
    mtxDiv[c]  = random(1, 4);
    mtxCnt[c]  = 0;
}

static void mtxInit() {
    spr.fillSprite(C_BLACK);
    for (int c = 0; c < MTX_COLS; c++) mtxResetCol(c, true);
}

static void mtxCell(int c, int row, char ch, uint16_t col) {
    if (row < 0 || row >= MTX_ROWS) return;
    int x = c * 12, y = row * 16;
    spr.fillRect(x, y, 12, 16, C_BLACK);
    if (ch) {
        spr.setCursor(x, y);
        spr.setTextColor(col);
        spr.print(ch);
    }
}

static char randGlyph() { return (char)random(33, 127); }

static void wMatrix() {
    spr.setFont(&lgfx::fonts::Font0);
    spr.setTextSize(2);
    spr.setTextDatum(lgfx::top_left);

    for (int c = 0; c < MTX_COLS; c++) {
        if (++mtxCnt[c] < mtxDiv[c]) continue;
        mtxCnt[c] = 0;

        int h = ++mtxHead[c];
        mtxCell(c, h,     randGlyph(), rgb(220, 255, 220));  // голова — почти белая
        mtxCell(c, h - 1, randGlyph(), C_GREEN);             // за ней — ярко-зелёный
        mtxCell(c, h - mtxLen[c] + 1, randGlyph(), rgb(0, 90, 0));  // хвост тускнеет
        mtxCell(c, h - mtxLen[c], 0, C_BLACK);               // конец хвоста — стереть

        if (h - mtxLen[c] > MTX_ROWS) mtxResetCol(c, false);
    }
}

// ═══ Виджет 7: Звёздное поле ══════════════════════════════════════════════════

static const int NUM_STARS = 90;
struct Star { float x, y, z; };
static Star stars[NUM_STARS];

static void starReset(Star& s) {
    s.x = random(-1000, 1001) / 1000.0f;
    s.y = random(-1000, 1001) / 1000.0f;
    s.z = random(300, 1000) / 1000.0f;
}

static void starsInit() {
    for (auto& s : stars) starReset(s);
}

static void wStarfield() {
    spr.fillSprite(C_BLACK);

    for (auto& s : stars) {
        s.z -= 0.018f;
        if (s.z <= 0.05f) { starReset(s); s.z = 1.0f; }

        int px = 120 + (int)(s.x / s.z * 110.0f);
        int py = 120 + (int)(s.y / s.z * 110.0f);
        if (px < 0 || px > 239 || py < 0 || py > 239) { starReset(s); s.z = 1.0f; continue; }

        uint8_t b = (uint8_t)constrain((1.1f - s.z) * 255.0f, 40.0f, 255.0f);
        int sz = s.z < 0.25f ? 3 : (s.z < 0.55f ? 2 : 1);
        spr.fillRect(px, py, sz, sz, rgb(b, b, b));
    }
}

// ═══ Диспетчер режимов ════════════════════════════════════════════════════════

struct Widget { void (*run)(); void (*init)(); const char* name; };

static const Widget WIDGETS[] = {
    { wAnalogClock,  nullptr,    "Analog clock"  },
    { wDigitalClock, nullptr,    "Digital clock" },
    { wCalendar,     nullptr,    "Calendar"      },
    { wRingLoader,   nullptr,    "Ring loader"   },
    { wProgressBar,  nullptr,    "Progress bar"  },
    { wGauge,        nullptr,    "Gauge"         },
    { wMatrix,       mtxInit,    "Matrix"        },
    { wStarfield,    starsInit,  "Starfield"     },
};
static const int NUM_WIDGETS = sizeof(WIDGETS) / sizeof(WIDGETS[0]);

static int currentWidget = 0;
static uint32_t modeSwitchMs = 0;
static bool blOn = true;

// ═══ Setup / Loop ═════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    Serial.println("MiniScreen widgets demo");

    pinMode(BTN1_PIN, INPUT_PULLUP);
    pinMode(BTN2_PIN, INPUT_PULLUP);
    randomSeed(esp_random());
    initClock();

    tft.init();
    tft.setRotation(0);
    tft.setBrightness(220);
    tft.fillScreen(C_BLACK);

    spr.setColorDepth(16);
    if (!spr.createSprite(240, 240)) {
        spr.setPsram(true);
        if (!spr.createSprite(240, 240)) {
            tft.setTextDatum(lgfx::middle_center);
            tft.setTextColor(C_RED);
            tft.setTextSize(2);
            tft.drawString("NO MEMORY", 120, 120);
            while (true) delay(1000);
        }
    }

    modeSwitchMs = millis();
    Serial.printf("Sprite OK. Widget: %s\n", WIDGETS[currentWidget].name);
}

void loop() {
    // Кнопки
    if (btnPressed(BTN1_PIN)) {
        currentWidget = (currentWidget + 1) % NUM_WIDGETS;
        if (WIDGETS[currentWidget].init) WIDGETS[currentWidget].init();
        modeSwitchMs = millis();
        Serial.printf("Widget: %s\n", WIDGETS[currentWidget].name);
    }
    if (btnPressed(BTN2_PIN)) {
        blOn = !blOn;
        tft.setBrightness(blOn ? 220 : 0);
    }

    // Рендер кадра
    WIDGETS[currentWidget].run();

    // Название виджета первые 1.2 сек после переключения
    if (millis() - modeSwitchMs < 1200) {
        spr.setFont(&lgfx::fonts::Font0);
        spr.setTextSize(1);
        spr.setTextDatum(lgfx::middle_center);
        int nameW = spr.textWidth(WIDGETS[currentWidget].name);
        spr.fillRoundRect(120 - nameW / 2 - 8, 204, nameW + 16, 18, 8, C_DGREY);
        spr.setTextColor(C_WHITE);
        spr.drawString(WIDGETS[currentWidget].name, 120, 213);
    }

    spr.pushSprite(0, 0);
}
