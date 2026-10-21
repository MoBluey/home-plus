#!/usr/bin/env python3
"""Draws Home+'s icon: tvOS's focus on a grid of apps. Four tiles on deep blue, three of them Liquid
Glass and one lifted and white, with a plus. Writes native/img/icon.png (80 px) and largeIcon.png
(130 px), the sizes webOS asks for, and docs/icon.png (512 px), for the Homebrew Channel's listing.
Needs Pillow:  python3 tools/make-icon.py   (or in the preview container)
"""
import os
from PIL import Image, ImageChops, ImageDraw, ImageEnhance, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
N = 2048  # drawn this size, then scaled down: smooth edges at every size


def blur(im, r):
    return im.filter(ImageFilter.GaussianBlur(r))


def rounded(mask, r):
    """Rounds a shape's corners: blur it, then cut it at half."""
    return blur(mask, r).point(lambda v: 255 if v >= 128 else 0)


def backdrop(n):
    """Deep blue, lighter at the top left, with a soft light behind the focused tile."""
    small = 64
    g = Image.new('RGB', (small, small))
    px = g.load()
    c0, c1 = (38, 92, 210), (6, 14, 48)
    for y in range(small):
        for x in range(small):
            t = min(1.0, max(0.0, (x * 0.55 + y) / (small * 1.55)))
            px[x, y] = tuple(int(c0[i] + (c1[i] - c0[i]) * t) for i in range(3))
    g = g.resize((n, n), Image.BICUBIC)
    light = Image.new('L', (n, n), 0)
    ImageDraw.Draw(light).ellipse([0, 0, 0.6 * n, 0.6 * n], fill=int(255 * 0.35))
    return Image.composite(Image.new('RGB', (n, n), (120, 170, 255)), g, blur(light, n * 0.12))


def glass(bg, mask, n, s, centre):
    """Liquid Glass over bg in mask: what's behind it magnified, frosted and lighter; a colour split
    and a bright rim at its edges; darker at its foot; a soft shadow under it."""
    out = Image.composite(Image.new('RGB', (n, n), (2, 2, 8)), bg,
                          ImageChops.offset(blur(mask, 36 * s), 0, int(30 * s)).point(lambda v: int(v * 0.6)))
    k = 1.22
    big = bg.resize((int(n * k), int(n * k)), Image.BICUBIC)
    ox, oy = int(centre[0] * s * (k - 1)), int(centre[1] * s * (k - 1))
    refr = blur(big.crop((ox, oy, ox + n, oy + n)), 10 * s)
    refr = ImageEnhance.Color(Image.blend(refr, Image.new('RGB', (n, n), (255, 255, 255)), 0.26)).enhance(1.35)
    bevel = ImageChops.subtract(mask, mask.filter(ImageFilter.MinFilter(int(34 * s) | 1)))
    r, g, b = refr.split()
    shift = int(9 * s)
    split = Image.merge('RGB', (ImageChops.offset(r, shift, 0), g, ImageChops.offset(b, -shift, 0)))
    out = Image.composite(Image.composite(split, refr, blur(bevel, 3 * s)), out, mask)
    foot = ImageChops.subtract(mask, ImageChops.offset(mask, 0, -int(40 * s)))
    out = Image.composite(Image.new('RGB', (n, n), (20, 18, 60)), out, blur(foot, 18 * s).point(lambda v: int(v * 0.45)))
    rim = ImageChops.subtract(mask, mask.filter(ImageFilter.MinFilter(int(9 * s) | 1)))
    light = ImageChops.invert(Image.linear_gradient('L').rotate(45).resize((n, n)))
    bright = ImageChops.multiply(rim, light.point(lambda v: min(255, int(40 + v * 1.1))))
    return Image.composite(Image.new('RGB', (n, n), (255, 255, 255)), out, blur(bright, 1.2 * s))


def draw(n=N):
    s = n / 1024  # the design is 1024 px, scaled
    out = backdrop(n)
    t, gap = 270, 56  # a tile's side, and the gap between tiles
    x0 = (1024 - 2 * t - gap) / 2
    for c, r in ((1, 0), (0, 1), (1, 1)):  # three tiles of glass
        m = Image.new('L', (n, n), 0)
        x, y = (x0 + c * (t + gap)) * s, (x0 + r * (t + gap)) * s
        ImageDraw.Draw(m).rounded_rectangle([x, y, x + t * s, y + t * s], radius=int(64 * s), fill=255)
        out = glass(out, m, n, s, (x0 + c * (t + gap) + t / 2, x0 + r * (t + gap) + t / 2))
    # the focused one: lifted, bigger and white, with a shadow and a plus
    ft = t * 1.14
    f = x0 + t / 2 - ft / 2 - 18
    m = Image.new('L', (n, n), 0)
    ImageDraw.Draw(m).rounded_rectangle([f * s, f * s, (f + ft) * s, (f + ft) * s], radius=int(72 * s), fill=255)
    out = Image.composite(Image.new('RGB', (n, n), (2, 2, 6)), out,
                          ImageChops.offset(blur(m, 40 * s), 0, int(34 * s)).point(lambda v: int(v * 0.75)))
    out = Image.composite(Image.new('RGB', (n, n), (250, 250, 252)), out, m)
    plus = Image.new('L', (n, n), 0)
    d = ImageDraw.Draw(plus)
    cx, cy, arm, half = (f + ft / 2) * s, (f + ft / 2) * s, 82 * s, 27 * s
    d.rounded_rectangle([cx - arm, cy - half, cx + arm, cy + half], radius=int(half), fill=255)
    d.rounded_rectangle([cx - half, cy - arm, cx + half, cy + arm], radius=int(half), fill=255)
    out = Image.composite(Image.new('RGB', (n, n), (28, 28, 34)), out, rounded(plus, 2 * s))
    tile = Image.new('L', (n, n), 0)
    ImageDraw.Draw(tile).rounded_rectangle([0, 0, n - 1, n - 1], radius=int(230 * s), fill=255)
    out = out.convert('RGBA')
    out.putalpha(tile)
    return out


def main():
    big = draw()
    for path, size in (('native/img/icon.png', 80), ('native/img/largeIcon.png', 130), ('docs/icon.png', 512)):
        dest = os.path.join(ROOT, path)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        big.resize((size, size), Image.LANCZOS).save(dest, optimize=True)
        print('wrote', path)


if __name__ == '__main__':
    main()
