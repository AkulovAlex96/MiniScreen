# Подключение GC9A01 к ESP32-S3

## Схема

```
GC9A01 display          ESP32-S3 board
─────────────────────────────────────────
GND  ──────────────────  GND
VCC  ──────────────────  3V3
SCL  ──────────────────  GPIO11  (SPI SCK)
SDA  ──────────────────  GPIO12  (SPI MOSI)
RES  ──────────────────  GPIO14  (Reset)
DC   ──────────────────  GPIO13  (Data/Command)
CS   ──────────────────  GPIO10  (Chip Select)
BLK  ──────────────────  GPIO21  (Backlight)
                         или напрямую в 3V3 для теста

BTN1 (один контакт) ───  GPIO1
BTN1 (другой контакт) ─  GND
BTN2 (один контакт) ───  GPIO2
BTN2 (другой контакт) ─  GND
```

## Особенности (найдено диагностикой)

- Панель GC9A01 требует **INVON** (`cfg.invert = true`), `rgb_order = false` — стандартно
- **ГЛАВНАЯ ЛОВУШКА LovyanGFX:** тип аргумента цвета определяет формат!
  `uint16_t` → RGB565, `uint32_t`/`int` → RGB888. `fillScreen(0xF800)` без явного
  uint16_t = тёмно-зелёный (RGB888), а не красный! Все цвета в коде — строго `uint16_t`
- Прошивка через USB-Serial/JTAG: `upload_speed = 460800` (на 921600 порт отваливался)

## Примечания

- **BLK**: для первого теста можно кинуть прямо в 3V3 — максимальная яркость без GPIO
- **Кнопки**: внутренняя подтяжка INPUT_PULLUP активирована в коде, внешние резисторы не нужны
- **Питание дисплея**: 3.3V, НЕ 5V (сгорит)
- **SPI**: только MOSI, CLK, CS, DC — MISO не нужен для дисплея

## Режимы демо

| BTN1 нажатие | Режим |
|---|---|
| 0 | Цикл цветов (RED/GREEN/BLUE/YELLOW/CYAN/MAGENTA/WHITE/ORANGE) |
| 1 | Анимированная радуга (HSV gradient) |
| 2 | Текст + инфо |

BTN2 — включить/выключить подсветку.

## Прошивка

```bash
# Установить PlatformIO
pip install platformio

# Прошить
cd firmware_demo
pio run --target upload

# Лог Serial
pio device monitor
```

Или через VS Code + PlatformIO Extension.

### Нативный USB ESP32-S3

Если плата с нативным USB (без CH340/CP2102) — при первой прошивке:
1. Зажать **BOOT**, нажать **RST**, отпустить RST, отпустить BOOT
2. Плата войдёт в Download Mode
3. `pio run --target upload`
4. После прошивки нажать RST
