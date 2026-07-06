# Инструменты для GIF: оптимизация, конвертация, поиск

`tools/convert_gif.py` кодирует кадры GIF как есть в C-массив байт — итоговый
размер во flash примерно равен размеру исходного GIF. Чем тяжелее исходник,
тем больше прошивка, поэтому тяжёлые/цветные GIF стоит сначала прогнать
через `gifsicle`, и только потом — через конвертер.

## gifsicle — оптимизация GIF

Установка:

```bash
sudo apt install gifsicle   # Debian/Ubuntu
brew install gifsicle       # macOS
```

Инфо о файле (кадры, цвета, размер после сжатия):

```bash
gifsicle --info file.gif
gifsicle --color-info --size-info file.gif
```

Полезные флаги (`gifsicle --help` для полного списка):

| Флаг | Что делает |
|---|---|
| `-O3` | Максимальная оптимизация (delta-кодирование между кадрами) |
| `-k, --colors N` | Урезать палитру до N цветов |
| `--lossy=N` | Лосси-сжатие (чем больше N, тем сильнее артефакты, но меньше размер) |
| `--resize WxH`, `--resize-fit WxH` | Ресайз кадра |
| `--crop X,Y+WxH` | Обрезать кадр по области |
| `-d, --delay N` | Задержка между кадрами, в сотых долях секунды |
| `#num1-num2` | Выбор диапазона кадров (в конце команды) |
| `--delete FRAMES` | Удалить кадры (например, каждый второй — уменьшить частоту) |
| `-b, --batch` | Изменить входные файлы на месте |

Пример: ужать GIF под круглый дисплей (240x240) перед конвертацией —

```bash
gifsicle -O3 --lossy=80 --colors 128 --resize-fit 240x240 \
  in.gif -o in_opt.gif

python tools/convert_gif.py ../../gif/in_opt.gif
```

## Конвертация Telegram-стикеров в GIF

Telegram отдаёт анимированные стикеры в `.webm` (видео) или `.tgs` (gzip'нутый
Lottie JSON) — конвертер их не понимает, нужен промежуточный шаг.

**`.webm` → `.gif`** (ffmpeg, через палитру — иначе цвета "грязные"):

```bash
ffmpeg -i sticker.webm -vf \
  "fps=25,scale=240:-1:flags=lanczos,split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse" \
  -loop 0 sticker.gif
```

**`.tgs` → `.gif`** (пакет `lottie`, рендерит через cairosvg):

```bash
pip install lottie cairosvg
lottie_convert.py sticker.tgs sticker.gif
```

## Где искать гифки и стикеры

- **giphy.com**, **tenor.com** — поиск готовых GIF по теме
- **ezgif.com** — веб-редактор без установки: обрезка кадров, ресайз, кроп,
  оптимизация, конвертация видео → GIF прямо в браузере
- **lottiefiles.com** — библиотека Lottie-анимаций (можно скачать как JSON/tgs
  и прогнать через `lottie_convert.py`)
- Стикеры из своих Telegram-чатов — сохранить как файл (`.webm`/`.tgs`) и
  сконвертировать по инструкции выше

## Чек-лист перед добавлением нового GIF в прошивку

1. `gifsicle -O3 --lossy=80 --resize-fit 240x240 ... -o opt.gif` — ужать
2. Положить `opt.gif` в `../gif/`
3. `python tools/convert_gif.py --all` — пересоздать заголовки + `gif_registry.h`
4. `pio run --target upload` и проверить `Flash:` в выводе сборки — суммарный
   размер всех GIF ограничен партишном (`board_build.partitions` в
   `platformio.ini`)
