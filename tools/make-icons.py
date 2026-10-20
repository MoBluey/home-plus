"""Draws the Home+ app icons (a 2x2 grid of glass tiles on a dark squircle)."""
import sys
from PIL import Image, ImageDraw, ImageFilter

def icon(size):
    S = size * 4
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # background: vertical gradient squircle
    bg = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(bg)
    for y in range(S):
        t = y / S
        d.line([(0, y), (S, y)], fill=(int(58 - 30 * t), int(52 - 30 * t), int(92 - 50 * t), 255))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.23), fill=255)
    img.paste(bg, (0, 0), mask)

    # four glass tiles
    pad, gap = int(S * 0.2), int(S * 0.07)
    w = (S - 2 * pad - gap) // 2
    colors = [(255, 255, 255, 235), (255, 255, 255, 120), (255, 255, 255, 120), (255, 255, 255, 120)]
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    tiles = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    td = ImageDraw.Draw(tiles)
    for i, c in enumerate(colors):
        x = pad + (i % 2) * (w + gap)
        y = pad + (i // 2) * (w + gap)
        box = [x, y, x + w, y + w]
        td.rounded_rectangle(box, radius=int(w * 0.28), fill=c)
        if i == 0:
            gd.rounded_rectangle(box, radius=int(w * 0.28), fill=(170, 150, 255, 200))
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.06))
    img = Image.alpha_composite(img, Image.composite(glow, Image.new("RGBA", (S, S)), mask))
    img = Image.alpha_composite(img, tiles)
    return img.resize((size, size), Image.LANCZOS)

out = sys.argv[1]
icon(80).save(f"{out}/icon.png")
icon(130).save(f"{out}/largeIcon.png")
icon(512).save(f"{out}/../../preview/icon-512.png")
