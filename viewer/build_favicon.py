#!/usr/bin/env python3
"""
Generate favicons into site/ from the single source FAVICON_SVG in app.py:
  favicon.svg, favicon.ico (16/32/48), favicon-32.png, apple-touch-icon.png (180).
Requires Pillow.

Usage:  python3 viewer/build_favicon.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import app  # noqa: E402

from PIL import Image, ImageDraw  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "site")
HEX = [(32, 12), (50, 22), (50, 42), (32, 52), (14, 42), (14, 22)]  # in 64x64 space


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def render(s):
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(img).rounded_rectangle([0, 0, s - 1, s - 1],
                                          radius=int(14 * s / 64), fill=(11, 11, 22, 255))
    grad = Image.new("RGBA", (s, s))
    px = grad.load()
    c0, c1 = (124, 124, 240), (34, 211, 238)
    for y in range(s):
        for x in range(s):
            px[x, y] = lerp(c0, c1, (x + y) / (2 * (s - 1))) + (255,)
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).polygon([(p[0] * s / 64, p[1] * s / 64) for p in HEX], fill=255)
    img.paste(grad, (0, 0), mask)
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "favicon.svg"), "w", encoding="utf-8") as f:
        f.write(app.FAVICON_SVG)
    master = render(256)
    master.resize((180, 180), Image.LANCZOS).save(os.path.join(OUT, "apple-touch-icon.png"))
    master.resize((32, 32), Image.LANCZOS).save(os.path.join(OUT, "favicon-32.png"))
    master.save(os.path.join(OUT, "favicon.ico"), sizes=[(16, 16), (32, 32), (48, 48)])
    print("wrote favicon.svg, favicon.ico, favicon-32.png, apple-touch-icon.png ->", OUT)


if __name__ == "__main__":
    main()
