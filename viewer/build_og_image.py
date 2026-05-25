#!/usr/bin/env python3
"""
Generate the social/OG preview image -> site/og.png (1200x630).
Reproducible branding asset for the landing. Requires Pillow + DejaVu fonts.

Usage:  python3 viewer/build_og_image.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

W, H = 1200, 630
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "site", "og.png")
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONTB = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    img = Image.new("RGB", (W, H), (11, 11, 22))
    d = ImageDraw.Draw(img)

    # vertical gradient background
    top, bot = (26, 27, 58), (8, 8, 18)
    for y in range(H):
        d.line([(0, y), (W, y)], fill=lerp(top, bot, y / H))

    # soft accent glow (concentric translucent ellipses, top-center)
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    for r, a in [(520, 26), (380, 30), (240, 34)]:
        gd.ellipse([W / 2 - r, -r * 0.7, W / 2 + r, r * 1.0], fill=(99, 102, 241, a))
    img = Image.alpha_composite(img.convert("RGBA"), glow).convert("RGB")
    d = ImageDraw.Draw(img)

    acc = (124, 124, 240)
    acc2 = (34, 211, 238)
    white = (245, 245, 250)
    mut = (170, 170, 200)

    brand = ImageFont.truetype(FONTB, 40)
    title = ImageFont.truetype(FONTB, 70)
    sub = ImageFont.truetype(FONT, 33)
    foot = ImageFont.truetype(FONT, 28)
    badge = ImageFont.truetype(FONTB, 24)

    M = 84
    # brand
    d.text((M, 70), "⬡  IA-SQL", font=brand, fill=acc)

    # title (two lines)
    d.text((M, 175), "A self-compiling", font=title, fill=white)
    d.text((M, 255), "knowledge base", font=title, fill=white)
    d.text((M, 335), "inside PostgreSQL", font=title, fill=acc2)

    # subtitle
    d.text((M, 445),
           "Insert documents → an LLM compiles a maintained,",
           font=sub, fill=mut)
    d.text((M, 487), "self-audited Markdown wiki.", font=sub, fill=mut)

    # footer
    d.text((M, 552), "github.com/feers77/iasql", font=foot, fill=white)

    # MIT badge (right)
    btxt = "Open source · MIT"
    bw = d.textlength(btxt, font=badge)
    bx0, by0 = W - M - bw - 36, 552
    d.rounded_rectangle([bx0, by0 - 6, bx0 + bw + 36, by0 + 34], radius=18,
                        outline=acc, width=2)
    d.text((bx0 + 18, by0), btxt, font=badge, fill=acc)

    img.save(OUT, "PNG")
    print("wrote", OUT, "%dx%d" % img.size)


if __name__ == "__main__":
    main()
