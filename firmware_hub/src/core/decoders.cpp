#include "decoders.h"

PNG png;
AnimatedGIF gif;

void decodersInit() {
    gif.begin(GIF_PALETTE_RGB565_LE);   // палитра little-endian, как spr.setSwapBytes(true)
}
