# firmware_hub — архитектура объединённой прошивки

Единая прошивка, в которой все экраны из тестовых проектов (`firmware_*`) живут
вместе как модули. Старые проекты остаются нетронутыми — на них удобно отлаживать
конкретный экран в изоляции, а стабильный результат портируется сюда.

## Ключевые решения

1. **Весь код экранов вшит в прошивку.** Динамическая подгрузка нативного кода на
   ESP32 нереалистична (ELF-загрузчик экспериментальный), и не нужна: все 10
   прототипов вместе — ~4500 строк, скомпилированный экран весит десятки КБ, а
   flash 16 МБ. Веб-интерфейс **включает/выключает** экраны из вшитого набора.
2. **«Докачивается» только контент**: гифки и прочие ассеты лежат на MicroSD и
   управляются через веб-файл-менеджер.
3. **Новые экраны доезжают по OTA**: `/update` в веб-морде принимает `firmware.bin`
   (PlatformIO: `.pio/build/esp32-s3/firmware.bin`) — перепрошивка без USB.
4. **Один общий фреймбуфер** (`spr`, 240×240×16bit = 115 КБ в PSRAM) принадлежит
   ядру; экраны только рисуют в него. Свои тяжёлые буферы экран обязан выделять в
   `onEnter()` и освобождать в `onExit()`.

## Структура

```
firmware_hub/
├── platformio.ini
├── ARCHITECTURE.md          # этот файл
└── src/
    ├── main.cpp             # setup/loop: только склейка модулей ядра
    ├── secrets.h(.example)  # Wi-Fi x2, дефолтные координаты, Mapbox token
    ├── core/
    │   ├── gfx.*            # LGFX-конфиг GC9A01, tft, spr, цвета, helpers
    │   ├── net.*            # Wi-Fi (2 сети, реконнект), общий TLS-клиент,
    │   │                    #   общий 48КБ буфер скачивания (PSRAM), fetchToBuffer
    │   ├── timesvc.*        # NTP, DateTime, календарные helpers
    │   ├── config.*         # NVS: hub-конфиг (активные экраны, координаты, TZ...)
    │   ├── buttons.*        # BTN1/BTN2 (short/long) + сенсорная TTP223
    │   ├── decoders.*       # общие экземпляры PNGdec и AnimatedGIF (они большие)
    │   ├── storage.*        # MicroSD на HSPI, монтирование с retry
    │   ├── screen.h         # интерфейс Screen — единственный контракт экрана
    │   ├── screen_manager.* # реестр, активный экран, pacing, авторотация
    │   └── webui.*          # веб: тумблеры экранов, настройки, файлы SD, OTA
    └── screens/
        ├── registry.cpp     # ЯВНЫЙ список всех экранов — единственная точка
        │                    #   регистрации: новый экран = 1 файл + 1 строка тут
        ├── clock_analog.cpp / clock_digital.cpp / calendar.cpp
        ├── demos.cpp        # ring loader, progress bar, gauge, matrix, starfield
        ├── radar.cpp        # радар самолётов (OpenSky + Mapbox-подложка)
        ├── gif_player.cpp   # GIF с MicroSD
        ├── clouds.cpp       # стилизованные тучи (Open-Meteo)
        ├── rain_radar.cpp   # реальный радар осадков (RainViewer)
        ├── city_map.cpp     # карта города + погода (Mapbox + Open-Meteo)
        └── sd_info.cpp      # инфо о MicroSD
```

## Контракт экрана (core/screen.h)

```cpp
class Screen {
public:
    virtual const char* id() const = 0;      // "radar" — ключ в NVS и веб-API
    virtual const char* title() const = 0;   // "Flight radar" — для веба/оверлея

    virtual void onEnter() {}                // выделить буферы, сбросить стейт
    virtual void onExit()  {}                // освободить ВСЁ тяжёлое (PSRAM общая)
    virtual void tick(uint32_t nowMs) = 0;   // кадр: рисовать в spr И самому pushSprite
    virtual uint32_t frameDelayMs() const { return 40; }   // pacing менеджера

    virtual void onButton(BtnEvent ev) {}    // только BTN1 short/long (см. ниже)
    virtual void getConfig(JsonObject out) {}        // настройки -> веб
    virtual void applyConfig(JsonObjectConst in) {}  // веб -> настройки (+свой NVS)
    virtual void onConfigChanged() {}        // hub-конфиг сменился (координаты и т.п.)
};
```

Правила:

- **Экран сам вызывает `spr.pushSprite(0,0)`** в конце кадра (так работали все
  прототипы — порт без переписывания). Экраны, рисующие редко (rain_radar,
  city_map), просто не пушат каждый tick.
- **Сеть экран дергает сам внутри `tick()`** по своим таймерам, через helpers из
  `net.h` (общий `secureClient`, общий буфер `netBuf`). Блокирующий фетч на 1-2 с
  внутри tick допустим — как в прототипах.
- **Состояние между заходами** можно держать в static'ах файла, но большие буферы
  (PSRAM-спрайты) — только между onEnter/onExit.

## Менеджер экранов

- Реестр — явный массив в `screens/registry.cpp`. Никакой магии саморегистрации:
  на embedded детерминизм важнее.
- Включённые экраны — CSV из id в NVS (`hub/en`), `*` = все. Активный — `hub/act`.
- Переключение: сенсорная кнопка или BTN2 long — следующий включённый экран;
  веб может активировать любой. Опциональная авторотация по таймеру (`rotateSec`).
- После переключения ~1.5 с поверх кадра рисуется оверлей с названием экрана
  (прямо в tft, поверх запушенного кадра).

## Кнопки

| Кнопка | Событие | Обработка |
|---|---|---|
| BTN2 (GPIO2) | short | ядро: яркость по кругу (220/130/60/0) |
| BTN2 | long | ядро: следующий экран |
| TTP223 (GPIO15) | касание | ядро: следующий экран |
| BTN1 (GPIO1) | short/long | **отдаётся активному экрану** (радар — рефреш, GIF — след./пред.) |

## Веб-API (порт 80, mDNS: miniscreen.local)

| Endpoint | Что делает |
|---|---|
| `GET /` | SPA-страница: тумблеры экранов, активация, настройки (генерируются из JSON) |
| `GET /api/screens` | список экранов: id, title, enabled, active |
| `POST /api/screens` | `{"enabled":[...]}` и/или `{"active":"id"}` |
| `GET/POST /api/config?screen=<id\|hub>` | настройки экрана / глобальные (координаты, TZ, ротация) |
| `GET /files` + upload/download/delete | файл-менеджер SD (из firmware_gif_sd) |
| `GET/POST /update` | OTA-прошивка |

Настройки в вебе рендерятся **генерически** из JSON `getConfig()` — экранам не
нужен свой HTML. Глобальные (hub): координаты дома (общие для radar/clouds/
rain_radar/city_map), часовой пояс, период авторотации.

## Память (бюджет)

| Что | Где | Размер |
|---|---|---|
| `spr` фреймбуфер | PSRAM | 115 КБ |
| `netBuf` (общий буфер скачивания) | PSRAM (ps_malloc) | 48 КБ |
| `mapSprite` радара | PSRAM, только пока радар активен | 115 КБ |
| PNGdec + AnimatedGIF (по 1 экз., общие) | BSS | ~80 КБ |
| heap для TLS-хендшейков | — | нужно ~50 КБ разом |

PNG/GIF-декодеры и буфер скачивания — **общие** (экран активен один за раз);
три копии, как было бы при слепом объединении прототипов, не влезли бы.

## Как добавить экран

1. `src/screens/my_screen.cpp`: класс-наследник `Screen` + функция-фабрика
   `Screen* myScreen()`.
2. `src/screens/registry.cpp`: добавить `myScreen()` в массив.
3. Всё: экран появится в вебе, в ротации кнопок, в NVS.

## Статус миграции

| Прототип | Экран(ы) в hub | Статус |
|---|---|---|
| firmware_widgets | clock_analog, clock_digital, calendar, demos (5 шт.) | ✅ (часы переведены на NTP) |
| firmware_radar | radar | ✅ (веб-настройки переехали в общий webui) |
| firmware_gif_sd | gif + файл-менеджер в webui | ✅ |
| firmware_clouds | clouds | ✅ |
| firmware_cloudradar | rain_radar | ✅ |
| firmware_map | city_map | ✅ |
| firmware_sdcard | sd_info | ✅ |
| firmware_mjpeg | — | не портирован (нужен внешний ffmpeg-сервер; добавится как экран позже) |
| firmware_gif (flash) | — | заменён gif_player'ом с SD |

## Возможное будущее

- Скриптовые экраны (Berry/Lua + API рисования) — по-настоящему динамические
  экраны без перепрошивки, но с потолком производительности (радар на 25 fps
  скрипт не потянет). Отдельный эксперимент.
- Докачка ассетов экранов (иконки, шрифты) на SD/LittleFS по манифесту.
