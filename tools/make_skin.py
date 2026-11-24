#!/usr/bin/env python3
"""Draws the MPC2500 emulator front panel (skin/skin.bmp) and writes skin/skin_layout.txt.

Usage: python3 -I tools/make_skin.py [outdir=skin] [--overlay <service-manual-cover.png>]

All element positions are measured off Akai's service-manual top-view drawing (units "mu" = pixels of that page
as displayed at 1413 px wide) and converted to the emulator's design space (DW x DH). The artwork is stylised, not
photographic: dark chassis, light-grey keycaps, labels printed above the keys like the real unit.
The layout file is what main.c reads for hit-testing, so moving a control here moves it in the emulator too.
Dynamic parts (pressed keys, LEDs, knob pointers, fader caps, wheel dimple, pads, LCD) are drawn live by main.c.
"""
import sys, os, math
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops

DW, DH = 1100, 860          # design space (what the emulator's logical window is, in "px")
R = 2                       # output pixels per design px (skin.bmp is DW*R x DH*R, crisp when the window is scaled)
U = 4                       # supersample factor for shapes (per design px)
K = 1.14                    # design px per manual unit
MX, MY = 14.2, 14.0         # margins


def tx(x): return (x - 275.0) * K + MX
def ty(y): return (y - 805.0) * K + MY
def M(x, y): return (tx(x), ty(y))
def ms(v): return v * K


FD = "/usr/share/fonts/truetype/liberation/"
_fonts = {}
def font(size, style="nb"):
    key = (size, style)
    if key not in _fonts:
        name = {"nb": "LiberationSansNarrow-Bold.ttf", "nr": "LiberationSansNarrow-Regular.ttf", "b": "LiberationSans-Bold.ttf",
                "bi": "LiberationSans-BoldItalic.ttf", "r": "LiberationSans-Regular.ttf", "nbi": "LiberationSansNarrow-BoldItalic.ttf",
                "dj": "../dejavu/DejaVuSans-Bold.ttf"}[style]
        _fonts[key] = ImageFont.truetype(FD + name, int(round(size * R)))
    return _fonts[key]


base = Image.new("RGB", (DW * U, DH * U), (14, 14, 16))
bd = ImageDraw.Draw(base)
layout = []          # lines for skin_layout.txt
texts = []           # (x, y, text, size, color, anchor, style) drawn on the final image
slanted = []         # (x, y, text, size, color, shear, style)
ops_after = []       # callables drawn on the final (R-scale) image


def P(v): return int(round(v * U))


def mask_rrect(w, h, r):
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, w - 1, h - 1), radius=max(0, r), fill=255)
    return m


def grad_rrect(x0, y0, x1, y1, r, top, bot):
    w, h = P(x1) - P(x0), P(y1) - P(y0)
    if w < 2 or h < 2: return
    g = Image.linear_gradient("L").resize((w, h))
    fill = Image.composite(Image.new("RGB", (w, h), bot), Image.new("RGB", (w, h), top), g)
    base.paste(fill, (P(x0), P(y0)), mask_rrect(w, h, P(r)))


def rrect(x0, y0, x1, y1, r, fill=None, outline=None, width=1):
    bd.rounded_rectangle((P(x0), P(y0), P(x1) - 1, P(y1) - 1), radius=P(r), fill=fill, outline=outline, width=max(1, int(width * U)) if outline else 0)


def shadow(x0, y0, x1, y1, r, off=(0, 1.5), blur=1.5, alpha=150):
    pad = int(blur * 3 + 4)
    w, h = P(x1 - x0) + 2 * pad * U, P(y1 - y0) + 2 * pad * U
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle((pad * U, pad * U, w - pad * U - 1, h - pad * U - 1), radius=P(r), fill=alpha)
    m = m.filter(ImageFilter.GaussianBlur(blur * U))
    base.paste(Image.new("RGB", (w, h), (0, 0, 0)), (P(x0 + off[0]) - pad * U, P(y0 + off[1]) - pad * U), m)


def circle(cx, cy, r, fill=None, outline=None, width=1):
    bd.ellipse((P(cx - r), P(cy - r), P(cx + r), P(cy + r)), fill=fill, outline=outline, width=max(1, int(width * U)) if outline else 0)


def radial_circle(cx, cy, r, center, edge):
    d = P(2 * r)
    if d < 2: return
    g = Image.radial_gradient("L").resize((d, d))
    fill = Image.composite(Image.new("RGB", (d, d), edge), Image.new("RGB", (d, d), center), g)
    m = Image.new("L", (d, d), 0); ImageDraw.Draw(m).ellipse((0, 0, d - 1, d - 1), fill=255)
    base.paste(fill, (P(cx - r), P(cy - r)), m)


def text(x, y, s, size=10, color=(238, 238, 240), anchor="mm", style="nb"):
    if any(ord(ch) > 255 for ch in s): style = "dj"      # Liberation lacks the arrow glyphs
    texts.append((x, y, s, size, color, anchor, style))


def rect_name(name, x0, y0, x1, y1):
    layout.append('btn "%s" %.1f %.1f %.1f %.1f' % (name, x0, y0, x1 - x0, y1 - y0))


def keycap(name, cx, cy, w, h, r=2.6, light=(226, 227, 230), dark=(166, 168, 174), reg=True):
    x0, x1, y0, y1 = cx - w / 2, cx + w / 2, cy - h / 2, cy + h / 2
    shadow(x0, y0, x1, y1, r, off=(0, 1.8), blur=1.3, alpha=190)
    rrect(x0 - 0.6, y0 - 0.6, x1 + 0.6, y1 + 0.6, r + 0.5, fill=(8, 8, 9))
    grad_rrect(x0, y0, x1, y1, r, light, dark)
    bd.line((P(x0 + r), P(y0 + 0.9), P(x1 - r), P(y0 + 0.9)), fill=(252, 252, 254), width=max(1, int(0.8 * U)))
    bd.line((P(x0 + r), P(y1 - 0.9), P(x1 - r), P(y1 - 0.9)), fill=(120, 122, 128), width=max(1, int(0.8 * U)))
    if reg and name: rect_name(name, x0, y0, x1, y1)


def red_keycap(name, cx, cy, w, h):
    keycap(name, cx, cy, w, h, light=(224, 70, 66), dark=(150, 28, 28))


def vents(x0, y0, w, gap, n, cols, colgap):
    for c in range(cols):
        for i in range(n):
            xx, yy = x0 + c * colgap, y0 + i * gap
            rrect(xx, yy, xx + w, yy + 5.2, 2.6, fill=(8, 8, 9))
            bd.line((P(xx + 2), P(yy + 5.6), P(xx + w - 2), P(yy + 5.6)), fill=(70, 72, 77), width=max(1, int(0.7 * U)))


def knob(cx, cy, r, red=False, name=None):
    """Static knob body + 11 scale dots; pointer is drawn live."""
    shadow(cx - r, cy - r, cx + r, cy + r, r, off=(0, 2.2), blur=2.4, alpha=210)
    circle(cx, cy, r + 1.3, fill=(6, 6, 7))
    circle(cx, cy, r, fill=(30, 31, 34), outline=(88, 90, 96), width=0.9)
    for a in range(0, 360, 12):                      # knurled skirt
        t0 = math.radians(a)
        bd.line((P(cx + (r - 0.9) * math.cos(t0)), P(cy + (r - 0.9) * math.sin(t0)), P(cx + (r - 3.6) * math.cos(t0)), P(cy + (r - 3.6) * math.sin(t0))),
                fill=(110, 112, 118), width=max(1, int(0.9 * U)))
    cr = r * 0.74
    if red: radial_circle(cx, cy, cr, (236, 78, 74), (138, 22, 24))
    else: radial_circle(cx, cy, cr, (246, 246, 248), (150, 152, 158))
    circle(cx, cy, cr, outline=(40, 40, 44), width=0.9)
    for i in range(11):                               # scale dots, 270 degree sweep
        a = math.radians(-225 + i * 27)
        circle(cx + (r + 5.2) * math.cos(a), cy + (r + 5.2) * math.sin(a), 0.85 if i not in (0, 10) else 1.25, fill=(220, 220, 224))
    return (cx - r, cy - r, 2 * r, 2 * r)


def led_socket(cx, cy, r=2.4):
    circle(cx, cy, r + 1.0, fill=(10, 10, 11), outline=(78, 80, 86), width=0.7)
    circle(cx, cy, r, fill=(52, 20, 20))


def cursor_cluster(cx, cy, w, h):
    x0, x1, y0, y1 = cx - w / 2, cx + w / 2, cy - h / 2, cy + h / 2
    shadow(x0, y0, x1, y1, 4, off=(0, 2), blur=1.6, alpha=200)
    rrect(x0 - 0.8, y0 - 0.8, x1 + 0.8, y1 + 0.8, 4.5, fill=(8, 8, 9))
    grad_rrect(x0, y0, x1, y1, 4, (150, 152, 158), (112, 114, 120))
    lw, mid = w * 0.24, w / 2
    # hexagonal centre key outline + dividers, like the real unit's arrow pad
    pts = [(cx - mid + lw, cy - h / 2 + 1), (cx + mid - lw, cy - h / 2 + 1), (cx + mid - lw + 5, cy), (cx + mid - lw, cy + h / 2 - 1),
           (cx - mid + lw, cy + h / 2 - 1), (cx - mid + lw - 5, cy)]
    bd.polygon([(P(a), P(b)) for a, b in pts], fill=(176, 178, 184), outline=(70, 36, 36))
    bd.line((P(cx - mid + lw - 5), P(cy), P(cx + mid - lw + 5), P(cy)), fill=(70, 36, 36), width=max(1, int(1.1 * U)))
    # arrow glyphs
    for dx, dy, ang in ((0, -h * 0.26, 90), (0, h * 0.26, 270), (-w * 0.36, 0, 180), (w * 0.36, 0, 0)):
        a = math.radians(ang); gx, gy = cx + dx, cy + dy; s = 4.4
        p1 = (gx + s * math.cos(a), gy - s * math.sin(a))
        p2 = (gx + s * math.cos(a + 2.5), gy - s * math.sin(a + 2.5))
        p3 = (gx + s * math.cos(a - 2.5), gy - s * math.sin(a - 2.5))
        bd.line([(P(p2[0]), P(p2[1])), (P(p1[0]), P(p1[1])), (P(p3[0]), P(p3[1]))], fill=(28, 28, 32), width=max(1, int(1.5 * U)), joint="curve")
    qa = w * 0.24
    rect_name("CURSOR UP", cx - mid + qa, y0, cx + mid - qa, cy)
    rect_name("CURSOR DOWN", cx - mid + qa, cy, cx + mid - qa, y1)
    rect_name("CURSOR LEFT", x0, y0, cx - mid + qa, y1)
    rect_name("CURSOR RIGHT", cx + mid - qa, y0, x1, y1)


# =========================================================================================================== body
bx0, by0 = M(275, 805); bx1, by1 = M(1215, 1535)
shadow(bx0 + 2, by0 + 4, bx1 - 2, by1 + 2, 18, off=(0, 3), blur=7, alpha=220)
rrect(bx0 - 1.4, by0 - 1.4, bx1 + 1.4, by1 + 1.4, 18, fill=(6, 6, 7))
grad_rrect(bx0, by0, bx1, by1, 17, (58, 60, 64), (36, 37, 40))
bd.line((P(bx0 + 17), P(by0 + 1.2), P(bx1 - 17), P(by0 + 1.2)), fill=(96, 99, 104), width=max(1, int(1.2 * U)))
# top rim (rear-panel strip) and bottom front lip
rx0, ry0 = M(283, 812); rx1, ry1 = M(1207, 836)
grad_rrect(rx0, ry0, rx1, ry1, 5, (28, 29, 32), (40, 41, 45))
lx0, ly0 = M(283, 1469); lx1, ly1 = M(1207, 1527)
grad_rrect(lx0, ly0, lx1, ly1, 6, (44, 45, 49), (22, 23, 25))
bd.line((P(lx0 + 6), P(ly0), P(lx1 - 6), P(ly0)), fill=(92, 94, 100), width=max(1, int(1.0 * U)))
sx0, sy = M(520, 1514); sx1, _ = M(1010, 1514)
rrect(sx0, sy - 1.2, sx1, sy + 1.2, 1.2, fill=(8, 8, 9))                       # CF card slot line
# side wings
for wx in (275, 1207):
    a, b = M(wx, 905); c, dd = M(wx + 8, 1480)
    grad_rrect(a, b, c, dd, 3, (30, 31, 34), (22, 23, 25))

# =========================================================================================================== left: vents, logo, Q-Link
vx, vy = M(299, 850)
vents(vx, vy, ms(30), ms(11.2), 5, 4, ms(40.5))
qb0x, qb0y = M(314.7, 986.6); qb1x, qb1y = M(457.9, 1002.4)
rrect(qb0x, qb0y, qb1x, qb1y, 2, fill=(22, 23, 25), outline=(178, 180, 184), width=1)
text((qb0x + qb1x) / 2, (qb0y + qb1y) / 2, "Q·link", 11, (232, 232, 236), "mm", "nb")
QCOLS = [(314.7, 382.0, 349.0, "Q3 AFTER (?)", "Q1 AFTER (?)", "q1", "q3", "Q3", "Q1"),
         (390.8, 458.2, 424.5, "Q4 AFTER (?)", "Q2 AFTER (?)", "q2", "q4", "Q4", "Q2")]
for ca, cb, cxm, nm_top, nm_bot, sname, kname, chip_top, chip_bot in QCOLS:
    x0, y0 = M(ca, 1008); x1, y1 = M(cb, 1391)
    rrect(x0 - 0.8, y0 - 0.8, x1 + 0.8, y1 + 0.8, 3, fill=(8, 8, 9))
    grad_rrect(x0, y0, x1, y1, 3, (28, 29, 32), (22, 23, 25))
    rrect(x0, y0, x1, y1, 3, outline=(168, 170, 176), width=1)
    cx, _ = M(cxm, 0)
    for chip, ymu in ((chip_top, 1030.5), (chip_bot, 1186.5)):           # small light chip labels above each AFTER key
        rrect(cx - 9, ty(ymu) - 5.5, cx + 9, ty(ymu) + 5.5, 2.5, fill=(214, 215, 219))
        text(cx, ty(ymu) + 0.3, chip, 9, (24, 24, 28), "mm", "nb")
    keycap(nm_top, cx, ty(1060), ms(32), ms(15.5), r=2.4, light=(196, 197, 200), dark=(142, 144, 150))
    text(cx, ty(1046.2), "AFTER", 9.5)
    kb = knob(cx, ty(1123.4), ms(20.5))
    layout.append("knob %s %.1f %.1f %.1f %.1f" % (kname, kb[0], kb[1], kb[2], kb[3]))
    text(tx(cxm - 25), ty(1150.5), "MIN", 7.5, (212, 212, 216), "mm", "nr"); text(tx(cxm + 25), ty(1150.5), "MAX", 7.5, (212, 212, 216), "mm", "nr")
    dxa, dya = M(ca + 4, 1168); dxb, _ = M(cb - 4, 1168)
    bd.line((P(dxa), P(dya), P(dxb), P(dya)), fill=(150, 152, 158), width=max(1, int(0.9 * U)))
    keycap(nm_bot, cx, ty(1216), ms(32), ms(15.5), r=2.4, light=(196, 197, 200), dark=(142, 144, 150))
    text(cx, ty(1201.6), "AFTER", 9.5)
    sy0, sy1 = ty(1256), ty(1381)
    rrect(cx - 3.4, sy0, cx + 3.4, sy1, 3, fill=(6, 6, 7), outline=(74, 76, 82), width=0.8)
    for i, ym in enumerate((1275, 1286, 1297, 1308, 1319, 1330, 1341, 1352, 1363, 1374)):
        yy = ty(ym); ln = ms(13) if i % 2 == 0 else ms(9)
        bd.line((P(cx - 6.2 - ln), P(yy), P(cx - 6.2), P(yy)), fill=(206, 206, 210), width=max(1, int(0.9 * U)))
        bd.line((P(cx + 6.2), P(yy), P(cx + 6.2 + ln), P(yy)), fill=(206, 206, 210), width=max(1, int(0.9 * U)))
    cap_h = ms(17)
    layout.append("slider %s %.1f %.1f %.1f %.1f" % (sname, cx - 15, sy0 + cap_h / 2 + 2, 30, (sy1 - sy0) - cap_h - 4))
keycap("ERASE", tx(349.5), ty(1434), ms(38.5), ms(29), r=3.2)
text(tx(349.5), ty(1410.5), "ERASE", 9.5)
keycap("NOTE REPEAT", tx(424.5), ty(1434), ms(38.5), ms(29), r=3.2)
text(tx(424.5), ty(1406), "NOTE\nREPEAT", 9.5)

# =========================================================================================================== LCD bezel + F keys
bz0x, bz0y = M(470, 822); bz1x, bz1y = M(905, 1012)
shadow(bz0x, bz0y, bz1x, bz1y, 5, off=(0, 3), blur=3, alpha=220)
rrect(bz0x - 1, bz0y - 1, bz1x + 1, bz1y + 1, 6, fill=(6, 6, 7))
grad_rrect(bz0x, bz0y, bz1x, bz1y, 5, (50, 52, 56), (30, 31, 34))
bd.line((P(bz0x + 5), P(bz0y + 1.2), P(bz1x - 5), P(bz0y + 1.2)), fill=(104, 107, 112), width=max(1, int(1.1 * U)))
GW = 440.0; GH = GW * 64 / 240
gcx, gcy = M(687.5, 0)[0], ty(912)
gx0, gy0 = gcx - GW / 2, gcy - GH / 2
rrect(gx0 - 8, gy0 - 8, gx0 + GW + 8, gy0 + GH + 8, 4, fill=(12, 12, 13), outline=(86, 88, 94), width=1)
rrect(gx0 - 3, gy0 - 3, gx0 + GW + 3, gy0 + GH + 3, 2, fill=(4, 4, 4))
layout.append("lcd %.1f %.1f %.1f %.1f" % (gx0, gy0, GW, GH))
fkx = [gx0 + (i + 0.5) * GW / 6 for i in range(6)]
for i in range(6):
    text(fkx[i], ty(982), "F%d" % (i + 1), 10.5, (236, 236, 238))
    keycap("F%d" % (i + 1), fkx[i], ty(1044), ms(31), ms(12.5), r=2.4)

# =========================================================================================================== pads
pp0x, pp0y = M(490, 1068); pp1x, pp1y = M(885, 1460)
shadow(pp0x, pp0y, pp1x, pp1y, 4, off=(0, 2), blur=2, alpha=200)
rrect(pp0x - 1.2, pp0y - 1.2, pp1x + 1.2, pp1y + 1.2, 5, fill=(70, 72, 78))
rrect(pp0x, pp0y, pp1x, pp1y, 4.5, fill=(10, 10, 12))
padcx = [557, 645, 731, 817]; padcy = [1133, 1220, 1304, 1391]; PSZ = ms(73)
PADSUB = {13: "SEQ EDIT", 14: "STEP EDIT", 15: "GRID EDIT", 16: "SONG", 9: "MIDI/SYNC", 10: "OTHER", 11: "", 12: "EFFECT",
          5: "RECORD", 6: "TRIM", 7: "PROGRAM", 8: "MIXER", 1: "SLIDER", 2: "LOAD", 3: "SAVE", 4: ""}
PADLET = {13: "Y\nZ", 14: "&\n#", 15: "-\n!", 16: "(\n)", 9: "Q\nR", 10: "S\nT", 11: "U\nV", 12: "W\nX", 5: "I\nJ", 6: "K\nL", 7: "M\nN", 8: "O\nP",
          1: "A\nB", 2: "C\nD", 3: "E\nF", 4: "G\nH"}
PADTOP = {13: "P ▶ LOOP", 14: "P ▶ TO", 15: "P ▶ FROM", 16: "P ▶ ALL"}
for n in range(1, 17):
    row, col = 3 - (n - 1) // 4, (n - 1) % 4
    cx, cy = tx(padcx[col]), ty(padcy[row])
    x0, y0, x1, y1 = cx - PSZ / 2, cy - PSZ / 2, cx + PSZ / 2, cy + PSZ / 2
    shadow(x0, y0, x1, y1, 2.4, off=(0, 2), blur=1.6, alpha=200)
    rrect(x0 - 1, y0 - 1, x1 + 1, y1 + 1, 3, fill=(4, 4, 5))
    grad_rrect(x0, y0, x1, y1, 2.4, (176, 177, 181), (146, 147, 152))
    bd.line((P(x0 + 2), P(y0 + 1), P(x1 - 2), P(y0 + 1)), fill=(214, 215, 219), width=max(1, int(0.9 * U)))
    layout.append("pad %d %.1f %.1f %.1f %.1f" % (n, x0, y0, PSZ, PSZ))
    text(x0 + 1, y0 - 7.5, "PAD %d" % n, 9, (236, 236, 238), "lm")
    if PADSUB[n]: text(x1 - 1, y0 - 7.5, PADSUB[n], 8.5, (232, 52, 48), "rm")
    if n in PADTOP: text(x0 + 1, y0 - 17, PADTOP[n], 6.5, (206, 206, 210), "lm", "nr")
    text(x0 - 7, cy - 1, PADLET[n], 6.8, (160, 162, 168), "mm", "nr") if col == 0 else None
    text(x1 + 6.5, cy - 1, PADLET[n + 1 if col < 3 else n], 6.8, (160, 162, 168), "mm", "nr") if col < 3 else None

# =========================================================================================================== top-right cluster
vx, vy = M(934, 850)
vents(vx, vy, ms(27), ms(8.2), 2, 6, ms(45.2))
# REC GAIN / MAIN VOLUME
kbr = knob(tx(1016.8), ty(909), ms(20.2), red=True)
layout.append("knob recgain %.1f %.1f %.1f %.1f" % kbr)
kbm = knob(tx(1115.6), ty(909), ms(20.2))
layout.append("knob mainvol %.1f %.1f %.1f %.1f" % kbm)
text(tx(1016.8), ty(874), "REC GAIN", 8.5); text(tx(1115.6), ty(874), "MAIN VOLUME", 8.5)
for xm in (1016.8, 1115.6):
    text(tx(xm - 24), ty(937), "MIN", 7, (212, 212, 216), "mm", "nr"); text(tx(xm + 24), ty(937), "MAX", 7, (212, 212, 216), "mm", "nr")
keycap("INPUT THRU", tx(952.8), ty(931), ms(26.6), ms(10.6), r=2.2)
text(tx(952.8), ty(916.5), "INPUT THRU", 8.5)
# PAD BANK
xs = [952.8, 1018.8, 1084.8, 1150.7]
text(tx(1062), ty(958.5), "PAD BANK", 8.5)
for xa, xb in ((937, 1033), (1091, 1166)):
    bd.line((P(tx(xa)), P(ty(958.5)), P(tx(xb)), P(ty(958.5))), fill=(176, 178, 184), width=max(1, int(0.9 * U)))
for x, nm, lab in zip(xs, ("PAD BANK A", "PAD BANK B", "PAD BANK C", "PAD BANK D"), "ABCD"):
    keycap(nm, tx(x), ty(983.2), ms(31), ms(14.2), r=2.6)
    text(tx(x), ty(970.4), lab, 9)
for x, nm, lab in zip(xs, ("FULL LEVEL", "16 LEVELS", "NEXT SEQ", "TRACK MUTE"), ("FULL LEVEL", "16 LEVELS", "NEXT SEQ", "TRACK MUTE")):
    keycap(nm, tx(x), ty(1021.7), ms(31), ms(14.2), r=2.6)
    text(tx(x), ty(1008.9), lab, 8.5)
# keypad
kpx = [929.4, 974.5, 1019.7]; kpy = [1080, 1113.7, 1147.4, 1181]
kp = [["7", "8", "9"], ["4", "5", "6"], ["1", "2", "3"], ["+/- (?)", "0", "ENTER"]]
kpl = [["7", "8", "9"], ["4", "5", "6"], ["1", "2", "3"], ["+/–", "0", "ENTER"]]
for r in range(4):
    for c in range(3):
        keycap(kp[r][c], tx(kpx[c]), ty(kpy[r]), ms(24.7), ms(10.6), r=2.2)
        text(tx(kpx[c]), ty(kpy[r] - 11.6), kpl[r][c], 9.5)
# DATA wheel
wcx, wcy, wr = tx(1122), ty(1110), ms(51)
text(wcx, wcy - wr - 11, "DATA", 9.5)
shadow(wcx - wr, wcy - wr, wcx + wr, wcy + wr, wr, off=(0, 3), blur=3.2, alpha=230)
circle(wcx, wcy, wr + 1.6, fill=(6, 6, 7))
radial_circle(wcx, wcy, wr, (196, 198, 204), (92, 94, 100))
for a in range(0, 360, 6):                        # grip ridges
    t0 = math.radians(a)
    bd.line((P(wcx + (wr - 1.2) * math.cos(t0)), P(wcy + (wr - 1.2) * math.sin(t0)), P(wcx + (wr * 0.80) * math.cos(t0)), P(wcy + (wr * 0.80) * math.sin(t0))),
            fill=(236, 238, 242) if (a // 6) % 2 == 0 else (84, 86, 92), width=max(1, int(0.9 * U)))
circle(wcx, wcy, wr * 0.78, fill=(110, 112, 118))
radial_circle(wcx, wcy, wr * 0.74, (224, 226, 231), (150, 152, 158))
circle(wcx, wcy, wr * 0.74, outline=(70, 72, 78), width=1.0)
layout.append("wheel %.1f %.1f %.1f" % (wcx, wcy, wr))
for x, nm, lab in ((1090, "DATA -", "–"), (1152.4, "DATA +", "+")):
    keycap(nm, tx(x), ty(1181), ms(26), ms(10.6), r=2.2)
    text(tx(x), ty(1181 - 11.6), lab, 11)
# MODE, TAP TEMPO, CURSOR
keycap("MODE", tx(929.4), ty(1240), ms(33), ms(15), r=2.8)
text(tx(929.4), ty(1225), "MODE", 9.5, (236, 52, 48))
keycap("TAP TEMPO", tx(1019.7), ty(1248.8), ms(38), ms(31), r=3.2)
text(tx(1019.7), ty(1224.5), "TAP TEMPO", 9.5)
text(tx(1122), ty(1212), "CURSOR", 9.5)
cursor_cluster(tx(1122), ty(1247.5), ms(76), ms(55))
for x, nm in ((929.4, "MAIN"), (984.5, "WINDOW"), (1040.5, "SHIFT"), (1151.6, "UNDO")):
    keycap(nm, tx(x), ty(1312.5), ms(31), ms(14.2), r=2.6)
    text(tx(x), ty(1299), nm, 9)

# =========================================================================================================== LOCATE / PLAY-REC panel
lp0x, lp0y = M(905.7, 1330.4); lp1x, lp1y = M(1177.8, 1458.7)
shadow(lp0x, lp0y, lp1x, lp1y, 3, off=(0, 2), blur=2, alpha=190)
rrect(lp0x - 1, lp0y - 1, lp1x + 1, lp1y + 1, 4, fill=(8, 8, 9))
grad_rrect(lp0x, lp0y, lp1x, lp1y, 3.5, (60, 62, 67), (44, 46, 50))
rrect(lp0x, lp0y, lp1x, lp1y, 3.5, outline=(98, 100, 106), width=0.9)
lxs = [929.4, 984.5, 1040.0, 1095.6, 1151.6]
text(tx(1041.5), ty(1338.5), "LOCATE", 8.5)
for xa, xb in ((912, 1010), (1073, 1172)):
    bd.line((P(tx(xa)), P(ty(1338.5)), P(tx(xb)), P(ty(1338.5))), fill=(150, 152, 158), width=max(1, int(0.8 * U)))
for x, nm in zip(lxs, ("STEP <", "STEP >", "GO TO", "BAR <<", "BAR >>")):
    keycap(nm, tx(x), ty(1370), ms(31), ms(14.2), r=2.6)
for x, g in ((lxs[0], "<"), (lxs[1], ">"), (lxs[3], "<<"), (lxs[4], ">>")):
    text(tx(x), ty(1355.5), g, 11)
text(tx(1040.0), ty(1355.5), "GO TO", 8.5)
text(tx((lxs[0] + lxs[1]) / 2), ty(1355.5), "STEP", 8.5); text(tx((lxs[3] + lxs[4]) / 2), ty(1355.5), "BAR", 8.5)
text(tx((lxs[0] + lxs[1]) / 2), ty(1382.5), "EVENT", 6.2, (196, 196, 200), "mm", "nr")
text(tx(lxs[3]), ty(1382.5), "START", 6.2, (196, 196, 200), "mm", "nr"); text(tx(lxs[4]), ty(1382.5), "END", 6.2, (196, 196, 200), "mm", "nr")
text(tx(1041.5), ty(1396), "PLAY/REC", 8.5)
for xa, xb in ((912, 995), (1090, 1172)):
    bd.line((P(tx(xa)), P(ty(1396)), P(tx(xb)), P(ty(1396))), fill=(150, 152, 158), width=max(1, int(0.8 * U)))
for x, nm in zip(lxs, ("REC", "OVER DUB", "STOP", "PLAY", "PLAY START")):
    if nm in ("REC", "OVER DUB"): red_keycap(nm, tx(x), ty(1432.5), ms(38), ms(27.5))
    else: keycap(nm, tx(x), ty(1432.5), ms(38), ms(27.5), r=3.0)
text(tx(lxs[0] + 4), ty(1411), "REC", 9, (236, 52, 48)); text(tx(lxs[1] + 4), ty(1411), "OVER\nDUB", 8.5, (236, 52, 48))
text(tx(lxs[2]), ty(1411), "STOP", 9); text(tx(lxs[3] + 4), ty(1411), "PLAY", 9); text(tx(lxs[4]), ty(1411), "PLAY\nSTART", 8.5)
for x, nm in ((lxs[0] - 12.5, "REC"), (lxs[1] - 13, "OVER DUB"), (lxs[3] - 12, "PLAY")):
    led_socket(tx(x), ty(1410.5), 2.3)
    layout.append('leddot "%s" %.1f %.1f %.1f' % (nm, tx(x), ty(1410.5), 2.3))

# =========================================================================================================== logos
# neutral labelling only: no manufacturer logo or marks (the emulated model is named in plain text)
text(tx(296), ty(930), "MPC2500", 30, (240, 240, 244), "lm", "b")
text(tx(297), ty(950), "EMULATOR", 11, (236, 236, 240), "lm", "b")
text(tx(384), ty(1500), "unofficial emulator", 9, (200, 202, 206), "mm", "b")


# =========================================================================================================== compose
def finish(outdir):
    out = base.resize((DW * R, DH * R), Image.LANCZOS)
    grain = Image.effect_noise((DW * R, DH * R), 16).convert("RGB")
    out = Image.blend(out, grain, 0.022)
    d = ImageDraw.Draw(out)
    for (x, y, s, size, col, anchor, style) in texts:
        f = font(size, style)
        if "\n" in s: d.multiline_text((x * R, y * R), s, font=f, fill=col, anchor=anchor, align="center", spacing=int(1.0 * R))
        else: d.text((x * R, y * R), s, font=f, fill=col, anchor=anchor)
    for (x, y, s, size, col, shear, style) in slanted:
        f = font(size, style)
        bb = d.textbbox((0, 0), s, font=f)
        w, h = bb[2] - bb[0] + int(shear * (bb[3] - bb[1])) + 8, bb[3] - bb[1] + 8
        layer = Image.new("L", (w, h), 0)
        ImageDraw.Draw(layer).text((4 - bb[0], 4 - bb[1]), s, font=f, fill=255)
        layer = layer.transform((w, h), Image.AFFINE, (1, shear, -shear * h, 0, 1, 0), Image.BICUBIC)
        out.paste(Image.new("RGB", (w, h), col), (int(x * R), int(y * R) - h // 2), layer)
    os.makedirs(outdir, exist_ok=True)
    out.save(os.path.join(outdir, "skin.bmp"))
    out.resize((DW, DH), Image.LANCZOS).save(os.path.join(outdir, "skin_preview.png"))
    with open(os.path.join(outdir, "skin_layout.txt"), "w") as f:
        f.write("# generated by tools/make_skin.py - do not edit by hand\n")
        f.write("size %d %d\nskinscale %d\n" % (DW, DH, R))
        f.write("\n".join(layout) + "\n")
    return out


def overlay(manual, outdir):
    im = Image.open(manual).convert("RGB"); k = im.size[0] / 1413.0
    d = ImageDraw.Draw(im)
    def to_manual(x, y): return (((x - MX) / K + 275.0) * k, ((y - MY) / K + 805.0) * k)
    for ln in layout:
        t = ln.split('"')
        if ln.startswith("btn") or ln.startswith("pad") or ln.startswith("knob") or ln.startswith("lcd") or ln.startswith("slider"):
            nums = [float(v) for v in (t[2] if ln.startswith("btn") else ln.split(None, 2 if ln.startswith(("pad", "knob", "slider")) else 1)[-1]).split()]
            if len(nums) == 4:
                a = to_manual(nums[0], nums[1]); b = to_manual(nums[0] + nums[2], nums[1] + nums[3])
                d.rectangle((a, b), outline=(255, 0, 0), width=3)
    x0, y0 = to_manual(0, 0)
    im.crop((int(275 * k) - 20, int(790 * k), int(1230 * k), int(1550 * k))).save(os.path.join(outdir, "overlay.png"))


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    outdir = args[0] if args else "skin"
    finish(outdir)
    import shutil; shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "led_map.txt"), os.path.join(outdir, "led_map.txt"))   # which OS LED bit lights which key (tracked in the repository, not generated)
    if "--overlay" in sys.argv:
        overlay(sys.argv[sys.argv.index("--overlay") + 1], outdir)
    print("wrote", outdir)
