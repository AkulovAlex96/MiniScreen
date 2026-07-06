# MiniScreen — настоящий радар/спутник облаков (RainViewer)

Не стилизация, а реальный снимок осадков/облачности: ESP32-S3 тянет метаданные
с api.rainviewer.com (без ключа), берёт самый свежий кадр радара, считает
нужный тайл вокруг `HOME_LAT/HOME_LON` и декодирует PNG (PNGdec) прямо в
спрайт.

## Запуск

Скопировать [src/secrets.h.example](src/secrets.h.example) в `src/secrets.h` и заполнить:

```cpp
#define WIFI_SSID_1 "приоритетная_сеть"
#define WIFI_PASS_1 "пароль"
#define WIFI_SSID_2 "резервная_сеть"     // например, точка доступа телефона
#define WIFI_PASS_2 "пароль"

#define HOME_LAT 55.7558   // центр тайла
#define HOME_LON 37.6173
```

(`secrets.h` в .gitignore). Ключ API не нужен. Прошить: `pio run --target upload`.

## Управление

- **BTN1** — рефреш тайла прямо сейчас
- **BTN2** — подсветка

## Особенности

- Тайл — глобальная сетка RainViewer, zoom максимум 7 (`ZOOM` в `main.cpp`,
  по умолчанию 6) — это область в сотни км, не карта улиц; масштаб погодного
  радара, а не города
- Опрос раз в 5 минут (`POLL_INTERVAL_MS`), сам радар RainViewer обновляется
  раз в 10 минут
- Прозрачные пиксели тайла (нет осадков) блендятся PNGdec'ом в фон-"небо"
  прямо при декодировании — без ручного маскирования
- **Атрибуция обязательна** по условиям RainViewer — текст "Weather data by
  RainViewer" всегда нарисован внизу экрана, не убирайте его
- Цветовая схема — `COLOR_SCHEME` (0-8) и `TILE_OPTIONS` (`smooth_snow`) в
  `main.cpp`, см. описание на rainviewer.com/api.html
