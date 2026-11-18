#!/usr/bin/env python3
"""Make magatama-wordmark.png and magatama-icon-1024.png from magatama-logo-dark.png (needs Pillow and numpy).

    python3 assets/branding/make_icons.py
"""
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent


def main() -> None:
    dark = np.asarray(Image.open(HERE / "magatama-logo-dark.png").convert("RGB")).astype(np.float32)
    ys, xs = np.where(dark.max(axis=2) > 8)
    x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()

    # The lettering on transparency: alpha from brightness (the logo is drawn on black), colour un-premultiplied.
    crop = dark[y0 - 8:y1 + 9, x0 - 8:x1 + 9]
    alpha = crop.max(axis=2)
    rgb = np.where(alpha[..., None] > 0, crop * 255 / np.maximum(alpha[..., None], 1), 0)
    wordmark = Image.fromarray(np.dstack([np.clip(rgb, 0, 255), alpha]).astype(np.uint8), "RGBA")
    wordmark.save(HERE / "magatama-wordmark.png", optimize=True)

    # The app icon: macOS grid, an 824-pixel black rounded square (radius 185) in a 1024 canvas, the lettering
    # across 82% of it with a soft purple glow behind.
    size, side = 1024, 824
    shape = Image.new("L", (side, side), 0)
    ImageDraw.Draw(shape).rounded_rectangle((0, 0, side - 1, side - 1), radius=185, fill=255)
    tile = Image.new("RGBA", (side, side), (0, 0, 0, 255))
    w = int(side * 0.82)
    h = int(wordmark.height * w / wordmark.width)
    letters = wordmark.resize((w, h), Image.LANCZOS)
    at = ((side - w) // 2, (side - h) // 2)
    glow = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    halo = Image.new("RGBA", (w, h), (124, 58, 237, 0))
    halo.putalpha(letters.getchannel("A").point(lambda v: int(v * 0.55)))
    glow.alpha_composite(halo, at)
    tile.alpha_composite(glow.filter(ImageFilter.GaussianBlur(18)))
    tile.alpha_composite(letters, at)
    tile.putalpha(Image.fromarray(np.minimum(np.asarray(tile.getchannel("A")), np.asarray(shape)).astype(np.uint8)))
    icon = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    icon.alpha_composite(tile, ((size - side) // 2, (size - side) // 2))
    icon.save(HERE / "magatama-icon-1024.png", optimize=True)


if __name__ == "__main__":
    main()
