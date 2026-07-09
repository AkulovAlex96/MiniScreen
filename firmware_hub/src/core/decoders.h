#pragma once
#include <PNGdec.h>
#include <AnimatedGIF.h>

// ─── Общие экземпляры декодеров ───────────────────────────────────────────────
// Объекты PNG и AnimatedGIF большие (десятки КБ BSS каждый) — по одному на всю
// прошивку, а не на экран. Экран активен один за раз, конфликтов нет; колбэки
// каждый экран передаёт свои в openRAM()/open().

extern PNG png;
extern AnimatedGIF gif;

void decodersInit();   // gif.begin(GIF_PALETTE_RGB565_LE)
