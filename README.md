# MiniScreen

Настольный виджет-девайс на ESP32-S3 с круглым дисплеем GC9A01 240x240.
Цель — своя плата, аккумулятор, LVGL-виджеты (часы/погода/акции), мелкая серия.
Полное описание концепции и схемотехники: [widget_display_project.md](widget_display_project.md).

## Структура

| Папка | Что это |
|---|---|
| [firmware_demo](firmware_demo/) | Базовый тест дисплея: цвета, радуга, текст. Подключение: [WIRING.md](firmware_demo/WIRING.md) |
| [firmware_widgets](firmware_widgets/) | 8 виджетов: аналоговые/цифровые часы, календарь, лоадеры, gauge, матрица, звёзды |
| [firmware_gif](firmware_gif/) | Проигрывание GIF из flash (конвертер в комплекте). Оптимизация/поиск гифок: [GIF_TOOLS.md](firmware_gif/GIF_TOOLS.md) |
| [firmware_mjpeg](firmware_mjpeg/) | Видео из сети: MJPEG по HTTP с ffmpeg-сервера (в т.ч. YouTube) |
| [firmware_radar](firmware_radar/) | Радар самолётов вокруг дома (OpenSky Network, без ключа) |
| [firmware_clouds](firmware_clouds/) | Стилизованные тучи по реальным данным погоды (Open-Meteo, без ключа) |
| [firmware_cloudradar](firmware_cloudradar/) | Настоящий радар/спутник облаков (RainViewer, без ключа) |
| [firmware_map](firmware_map/) | Карта города + погода текстом (Mapbox — нужен ключ) |
| [firmware_sdcard](firmware_sdcard/) | Инфо о MicroSD: объём, кол-во файлов, круговой прогресс-бар |
| [gif](gif/) | Исходные гифки |
| [Img](Img/), [Test_board](Test_board/) | Схемы блоков будущей платы (EasyEDA) |

## Железо (текущий брэдборд)

- ESP32-S3 DevKit (N16R8: 16 МБ flash, 8 МБ PSRAM)
- Дисплей GC9A01 1.28" 240x240 (SPI: SCK=11, MOSI=12, CS=10, DC=13, RST=14, BL=21)
- 2 кнопки: GPIO1, GPIO2 (на GND, внутренние pull-up)

## Сборка

Все прошивки — PlatformIO: `pio run --target upload` в папке прошивки.

Грабли, на которые уже наступили:
- **LovyanGFX:** тип цвета определяет формат — `uint16_t`=RGB565, `uint32_t`=RGB888.
  `fillScreen(0xF800)` без явного uint16_t — это тёмно-зелёный RGB888, не красный!
- **GC9A01:** нужен `invert=true`, `rgb_order=false`
- **Прошивка по USB-JTAG:** `upload_speed = 460800`, при зависании — BOOT+RST
- **firmware_gif:** быстрое добавление GIF:
  1) положить новые `.gif` в папку `gif/`
  2) выполнить `cd firmware_gif && python tools/convert_gif.py --all`
  3) прошить `pio run --target upload` — проигрыватель автоматически подхватит все сгенерированные GIF из `src/gif_registry.h`
- **firmware_mjpeg:** скопировать `src/secrets.h.example` → `src/secrets.h`, вписать Wi-Fi
- **firmware_radar/clouds/cloudradar/map:** тот же паттерн `secrets.h.example` →
  `secrets.h`, но с 2 Wi-Fi сетями (приоритетная + резервная) и домашними
  координатами. Ключ нужен только **firmware_map** (Mapbox access token,
  free tier). **firmware_cloudradar** обязан показывать на экране атрибуцию
  "Weather data by RainViewer" — так требуют условия использования их API
- **firmware_sdcard:** MicroSD на отдельной SPI-шине (HSPI/SPI3), не общей с
  дисплеем (LovyanGFX держит SPI2 монопольно) — CS=GPIO38, SCK=GPIO39,
  MOSI=GPIO40, MISO=GPIO46. Опциональная сенсорная кнопка (TTP223) на GPIO15
  дублирует BTN1 (рефреш)
