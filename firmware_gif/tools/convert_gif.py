"""
Конвертация GIF под круглый дисплей 240x240:
  1. Композитинг кадров (учет transparency/disposal)
  2. Кроп по центру до квадрата + ресайз до нужного размера
  3. Сохранение оптимизированного GIF (для проверки на ПК)
  4. Генерация C-заголовка с массивом байт для прошивки
  5. Автогенерация реестра src/gif_registry.h для всех *_gif.h

Примеры:
  python tools/convert_gif.py ../../gif/racoon.gif
  python tools/convert_gif.py ../../gif/racoon.gif ../../gif/Eltexlogo_blue.gif
  python tools/convert_gif.py --all
  python tools/convert_gif.py --all --size 200
"""

import argparse
import re
from pathlib import Path

from PIL import Image, ImageSequence


def make_symbol(stem: str) -> str:
    symbol = re.sub(r"[^0-9a-zA-Z_]", "_", stem)
    if not symbol:
        symbol = "gif"
    if symbol[0].isdigit():
        symbol = f"gif_{symbol}"
    return f"{symbol}_gif"


def convert_one(src_path: Path, size: int, tools_dir: Path, src_dir: Path) -> None:
    im = Image.open(src_path)
    print(f"Вход: {src_path.name} {im.size}, {im.n_frames} кадров")

    frames = []
    durations = []
    for frame in ImageSequence.Iterator(im):
        durations.append(frame.info.get("duration", 100))
        # Pillow уже композитит кадр с учётом disposal-метода сам,
        # поэтому здесь нужен только чистый чёрный фон под альфаканал,
        # а не накопление кадров поверх одного и того же canvas
        # (иначе прозрачные области не стирают предыдущие кадры и
        # получаются "хвосты"/призраки на анимации).
        rgba = frame.convert("RGBA")
        canvas = Image.new("RGBA", im.size, (0, 0, 0, 255))
        canvas.alpha_composite(rgba)

        w, h = canvas.size
        side = min(w, h)
        box = (
            (w - side) // 2,
            (h - side) // 2,
            (w - side) // 2 + side,
            (h - side) // 2 + side,
        )
        small = canvas.crop(box).resize((size, size), Image.LANCZOS)
        frames.append(
            small.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT)
        )

    out_gif = tools_dir / f"{src_path.stem}_{size}.gif"
    frames[0].save(
        out_gif,
        save_all=True,
        append_images=frames[1:],
        duration=durations,
        loop=0,
        optimize=True,
    )
    data = out_gif.read_bytes()
    print(f"GIF {size}x{size}: {out_gif.name}, {len(data)} байт")

    out_h = src_dir / f"{src_path.stem}_gif.h"
    symbol = make_symbol(src_path.stem)
    with open(out_h, "w", encoding="utf-8") as f:
        f.write(f"// Сгенерировано convert_gif.py из {src_path.name}\n")
        f.write(f"// {size}x{size}, {len(frames)} кадров\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"const uint32_t {symbol}_len = {len(data)};\n")
        f.write(f"const uint8_t {symbol}[] = {{\n")
        for i in range(0, len(data), 16):
            chunk = ", ".join(f"0x{b:02X}" for b in data[i : i + 16])
            f.write(f"  {chunk},\n")
        f.write("};\n")
    print(f"Заголовок: {out_h}")


def parse_header_symbols(header_path: Path) -> tuple[str, str] | None:
    text = header_path.read_text(encoding="utf-8", errors="ignore")
    data_match = re.search(r"const\s+uint8_t\s+([A-Za-z_][A-Za-z0-9_]*)\[\]", text)
    len_match = re.search(r"const\s+uint32_t\s+([A-Za-z_][A-Za-z0-9_]*)", text)
    if not data_match or not len_match:
        return None
    return data_match.group(1), len_match.group(1)


def generate_registry(src_dir: Path) -> None:
    headers = sorted(src_dir.glob("*_gif.h"))
    entries = []
    for header in headers:
        parsed = parse_header_symbols(header)
        if not parsed:
            continue
        data_name, len_name = parsed
        stem = header.stem[: -len("_gif")] if header.stem.endswith("_gif") else header.stem
        entries.append((header.name, stem, data_name, len_name))

    out = src_dir / "gif_registry.h"
    with open(out, "w", encoding="utf-8") as f:
        f.write("// Сгенерировано convert_gif.py\n")
        f.write("#pragma once\n")
        f.write("#include <stddef.h>\n")
        f.write("#include <stdint.h>\n\n")
        f.write("struct GifAsset {\n")
        f.write("  const char* name;\n")
        f.write("  const uint8_t* data;\n")
        f.write("  uint32_t len;\n")
        f.write("};\n\n")

        for header_name, _, _, _ in entries:
            f.write(f"#include \"{header_name}\"\n")

        f.write("\n")
        f.write("static const GifAsset kGifAssets[] = {\n")
        for _, stem, data_name, len_name in entries:
            f.write(f"  {{\"{stem}\", {data_name}, {len_name}}},\n")
        f.write("};\n\n")
        f.write("static const size_t kGifAssetsCount = sizeof(kGifAssets) / sizeof(kGifAssets[0]);\n")
    print(f"Реестр: {out} ({len(entries)} GIF)")


def collect_inputs(args: argparse.Namespace, gif_dir: Path) -> list[Path]:
    sources: list[Path] = []
    if args.all:
        sources.extend(sorted(gif_dir.glob("*.gif")))
    for raw in args.inputs:
        p = Path(raw)
        if not p.is_absolute():
            p = (Path.cwd() / p).resolve()
        sources.append(p)

    unique_sources = []
    seen = set()
    for path in sources:
        key = str(path)
        if key in seen:
            continue
        seen.add(key)
        unique_sources.append(path)
    return unique_sources


def main() -> int:
    parser = argparse.ArgumentParser(description="Конвертер GIF -> C header + реестр")
    parser.add_argument("inputs", nargs="*", help="Пути к gif-файлам")
    parser.add_argument("--all", action="store_true", help="Конвертировать все *.gif из ../../gif")
    parser.add_argument("--size", type=int, default=240, help="Размер итогового квадрата")
    args = parser.parse_args()

    tools_dir = Path(__file__).resolve().parent
    src_dir = tools_dir.parent / "src"
    gif_dir = (tools_dir / "../../gif").resolve()

    inputs = collect_inputs(args, gif_dir)
    if not inputs:
        inputs = [(gif_dir / "racoon.gif").resolve()]

    converted = 0
    for src_path in inputs:
        if src_path.suffix.lower() != ".gif":
            print(f"Пропуск (не GIF): {src_path}")
            continue
        if not src_path.exists():
            print(f"Файл не найден: {src_path}")
            continue
        convert_one(src_path, args.size, tools_dir, src_dir)
        converted += 1

    if converted == 0:
        print("Нет файлов для конвертации")
        return 1

    generate_registry(src_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
