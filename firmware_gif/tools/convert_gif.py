"""
Конвертация GIF под круглый дисплей 240x240:
  1. Композитинг кадров (учёт disposal/transparency)
  2. Кроп по центру до квадрата + ресайз до 240x240
  3. Сохранение оптимизированного GIF (для проверки на ПК)
  4. Генерация C-заголовка с массивом байт для прошивки

Использование: python convert_gif.py <input.gif> [размер]
"""
import sys
from pathlib import Path

from PIL import Image, ImageSequence

SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 240
src_path = Path(sys.argv[1] if len(sys.argv) > 1 else "../../gif/racoon.gif")
here = Path(__file__).parent

im = Image.open(src_path)
print(f"Вход: {src_path.name} {im.size}, {im.n_frames} кадров")

# 1-2. Композитинг + кроп + ресайз
frames = []
durations = []
canvas = None
for frame in ImageSequence.Iterator(im):
    durations.append(frame.info.get("duration", 100))
    rgba = frame.convert("RGBA")
    if canvas is None:
        canvas = Image.new("RGBA", im.size, (0, 0, 0, 255))
    canvas.alpha_composite(rgba)

    w, h = canvas.size
    side = min(w, h)
    box = ((w - side) // 2, (h - side) // 2,
           (w - side) // 2 + side, (h - side) // 2 + side)
    small = canvas.crop(box).resize((SIZE, SIZE), Image.LANCZOS)
    frames.append(small.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT))

# 3. Сохраняем итоговый GIF
out_gif = here / f"{src_path.stem}_{SIZE}.gif"
frames[0].save(
    out_gif, save_all=True, append_images=frames[1:],
    duration=durations, loop=0, optimize=True,
)
data = out_gif.read_bytes()
print(f"GIF {SIZE}x{SIZE}: {out_gif.name}, {len(data)} байт")

# 4. Генерируем заголовок
out_h = here.parent / "src" / f"{src_path.stem}_gif.h"
name = f"{src_path.stem}_gif"
with open(out_h, "w") as f:
    f.write(f"// Сгенерировано convert_gif.py из {src_path.name}\n")
    f.write(f"// {SIZE}x{SIZE}, {len(frames)} кадров\n")
    f.write("#pragma once\n#include <stdint.h>\n\n")
    f.write(f"const uint32_t {name}_len = {len(data)};\n")
    f.write(f"const uint8_t {name}[] = {{\n")
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i:i + 16])
        f.write(f"  {chunk},\n")
    f.write("};\n")
print(f"Заголовок: {out_h}")
