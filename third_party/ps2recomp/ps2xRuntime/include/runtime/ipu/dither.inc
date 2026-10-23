__ri void ipu_dither_reference(const macroblock_rgb32 &rgb32, macroblock_rgb16 &rgb16, int dte)
{
    if (dte) {
        // I'm guessing values are rounded down when clamping.
        const int dither_coefficient[4][4] = {
            {-4, 0, -3, 1},
            {2, -2, 3, -1},
            {-3, 1, -4, 0},
            {3, -1, 2, -2},
        };
        for (int i = 0; i < 16; ++i) {
            for (int j = 0; j < 16; ++j) {
                const int dither = dither_coefficient[i & 3][j & 3];
                const int r = std::max(0, std::min(rgb32.c[i][j].r + dither, 255));
                const int g = std::max(0, std::min(rgb32.c[i][j].g + dither, 255));
                const int b = std::max(0, std::min(rgb32.c[i][j].b + dither, 255));

                rgb16.c[i][j].r = r >> 3;
                rgb16.c[i][j].g = g >> 3;
                rgb16.c[i][j].b = b >> 3;
                rgb16.c[i][j].a = rgb32.c[i][j].a == 0x40;
            }
        }
    } else {
        for (int i = 0; i < 16; ++i) {
            for (int j = 0; j < 16; ++j) {
                rgb16.c[i][j].r = rgb32.c[i][j].r >> 3;
                rgb16.c[i][j].g = rgb32.c[i][j].g >> 3;
                rgb16.c[i][j].b = rgb32.c[i][j].b >> 3;
                rgb16.c[i][j].a = rgb32.c[i][j].a == 0x40;
            }
        }
    }
}


void ipu_dither(const macroblock_rgb32& a, macroblock_rgb16& b, int d) { ipu_dither_reference(a,b,d); }
