#!/usr/bin/env python3
"""Draws the application icon (packaging/icon.png, 512x512): a stylised sampler - dark body, lime LCD, a 4x4 pad grid. Original artwork, no third-party logos."""
import os
from PIL import Image, ImageDraw
S = 512
im = Image.new("RGBA", (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
d.rounded_rectangle((16, 16, S - 16, S - 16), 76, fill=(34, 36, 40, 255), outline=(70, 74, 82, 255), width=6)
d.rounded_rectangle((60, 60, S - 60, 190), 22, fill=(18, 20, 22, 255))
d.rounded_rectangle((78, 76, S - 78, 174), 12, fill=(170, 200, 68, 255))
for i, row in enumerate(("MPC 2500", "Pgm:KIT  120.0")):                   # a few LCD-like pixel rows
    y = 94 + i * 38
    for x in range(98, S - 98, 14): d.rectangle((x, y, x + 8, y + 18), fill=(24, 38, 12, 255 if (x // 14 + i) % 3 else 90))
pad = 74; gap = 16; x0 = (S - (4 * pad + 3 * gap)) // 2; y0 = 222
for r in range(4):
    for c in range(4):
        x = x0 + c * (pad + gap); y = y0 + r * (pad + gap)
        d.rounded_rectangle((x, y, x + pad, y + pad), 10, fill=(176, 178, 184, 255) if (r, c) != (1, 2) else (255, 90, 70, 255))
im.save(os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon.png")); print("wrote packaging/icon.png")
