#!/usr/bin/env python3
"""Regenerates the screenshots used by README.md and QUICKSTART.md in docs/images/ by running the emulator itself.

  python3 tools/make_doc_images.py            (needs: Linux, the built emu/ binaries, the firmware files under archive/, card/card.img with an
                                               AUTOLOAD folder and card/test_card.img, Pillow)

Everything is produced from the real emulated firmware output (the LCD pictures are the T6963C frame buffer rendered like the live window does)."""
import os, subprocess, sys, tempfile, shutil
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "images"); os.makedirs(OUT, exist_ok=True)
HL = os.path.join(ROOT, "emu", "mpc2500emu_hl"); WIN = os.path.join(ROOT, "emu", "mpc2500emu")
FW = os.path.join(ROOT, "archive")
STOCK = os.path.join(FW, "v124", "mpc2500.bin")
KIT_PRELUDE = ("tap 0 3;wait 20000000;padhit 2 100;wait 30000000;tap 2 8;wait 30000000;tap 3 8;wait 1600000000;tap 0 2;wait 30000000;tap 0 3;wait 20000000;"
               "padhit 7 100;wait 30000000;tap 4 4;wait 20000000")
DARK, LIGHT = (0x18, 0x26, 0x0C), (0xAA, 0xC8, 0x44)


def lcd_png(binfile, name, scale=3):
    d = open(binfile, "rb").read()
    im = Image.new("RGB", (240, 64))
    px = im.load()
    for y in range(64):
        for bx in range(30):
            b = d[y * 30 + bx]
            for k in range(8): px[bx * 8 + k, y] = DARK if (b >> (7 - k)) & 1 else LIGHT
    im = im.resize((240 * scale, 64 * scale), Image.NEAREST)
    framed = Image.new("RGB", (im.width + 24, im.height + 24), (20, 22, 24)); framed.paste(im, (12, 12))
    framed.save(os.path.join(OUT, name)); print("wrote", name)


def run_exp(fw, forks, boot=300_000_000, card="test_card.img", prelude=None, env=None):
    tmp = tempfile.mkdtemp(prefix="docimg_")
    e = dict(os.environ, CARD=os.path.join(ROOT, "card", card), SODIMM_MB="64"); e.update(env or {})
    args = [HL, fw, "--exp", tmp, str(boot)] + (["PRELUDE=" + prelude] if prelude else []) + forks
    subprocess.run(args, env=e, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    return tmp


def screens():
    # --- stock OS: load page, slider page, mixer, program filter page, boot with AUTOLOAD ---
    t = run_exp(STOCK, ["load=press 0 3;wait 20000000;padhit 2 100;wait 20000000;release 0 3;wait 80000000;dump a",
                        "mixer=press 0 3;wait 20000000;padhit 8 100;wait 20000000;release 0 3;wait 80000000;dump a",
                        "main=wait 1000000;dump a"])
    lcd_png(f"{t}/load_a.bin", "lcd_load.png"); lcd_png(f"{t}/mixer_a.bin", "lcd_mixer.png"); lcd_png(f"{t}/main_a.bin", "lcd_main_empty.png")
    t = run_exp(STOCK, ["filter=tap 3 9;wait 30000000;padhit 5 100;wait 40000000;tap 3 2;wait 15000000;tap 3 2;wait 15000000;wheel 1 1;wait 40000000;dump a",
                        "slider=press 0 3;wait 20000000;padhit 1 100;wait 20000000;release 0 3;wait 80000000;tap 3 2;wait 20000000;padhit 5 100;wait 40000000;"
                        "tap 3 2;wait 20000000;wheel -1 1;wait 40000000;tap 3 2;wait 20000000;wheel 1 3;wait 40000000;dump a"], prelude=KIT_PRELUDE)
    lcd_png(f"{t}/filter_a.bin", "lcd_program_filter.png"); lcd_png(f"{t}/slider_a.bin", "lcd_slider.png")
    t = run_exp(STOCK, ["loading=wait 200000000;dump a", "done=wait 800000000;dump a"], boot=1000, card="card.img")
    lcd_png(f"{t}/loading_a.bin", "lcd_autoload_loading.png"); lcd_png(f"{t}/done_a.bin", "lcd_autoload_done.png")
    # --- the other firmware families ---
    for fname, path, png, env in (("jv313", os.path.join(FW, "MPC2500", "mpc2500_jv313.bin"), "lcd_custom_os_fits.png", {}),
                                  ("xl390", os.path.join(FW, "MPC2500", "mpc2500_xl390.osx"), "lcd_xl_os.png", {"FLASH_MODE": "full"}),
                                  ("light100", os.path.join(FW, "MPC2500", "mpc2500_light100.osx"), "lcd_xl_os2.png", {"FLASH_MODE": "full"}),
                                  ("jv263", os.path.join(FW, "MPC2500", "mpc2500_jv263.os3"), "lcd_xl_os3.png", {"FLASH_MODE": "full"})):
        if os.path.exists(path):
            t = run_exp(path, [f"{fname}=wait 1000000;dump a"], boot=400_000_000, env=env)
            lcd_png(f"{t}/{fname}_a.bin", png)


def panel():
    """Full front-panel screenshot from the live window (dummy video driver), stock OS with the AUTOLOAD kit loaded (Tab held = fast-forward)."""
    shot = os.path.join(ROOT, "live_shot.bmp")
    if os.path.exists(shot): os.remove(shot)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", LIVE_EVENTS="5:d:Tab;1900:u:Tab", LIVE_FRAMES="2100", DISCLAIMER_TEST="accept")  # test hook: skip the start-up notice
    subprocess.run([WIN, STOCK], cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
    im = Image.open(shot).convert("RGB"); im.save(os.path.join(OUT, "panel.png")); os.remove(shot); print("wrote panel.png")
    return im


def keymap(panel_im):
    """The panel with the keyboard key of every button drawn on it."""
    layout, pads = {}, {}
    for line in open(os.path.join(ROOT, "skin", "skin_layout.txt")):
        p = line.split()
        if line.startswith("btn "):
            q = line.split('"'); v = q[2].split(); layout[q[1]] = tuple(float(x) for x in v[:4])
        elif line.startswith("pad "): pads[int(p[1])] = tuple(float(x) for x in p[2:6])
    cellname, keys, padkeys = {}, {}, {}
    for line in open(os.path.join(ROOT, "panel_map.txt")):
        if line.startswith("cell "):
            p = line.rstrip("\n").split(" ", 3); cellname[(int(p[1]), int(p[2]))] = p[3]
        elif line.startswith("key "):
            rest = line.rstrip("\n")[4:].split(); keys[(int(rest[-2]), int(rest[-1]))] = " ".join(rest[:-2])
        elif line.startswith("padkey "):
            p = line.rstrip("\n").split(" ", 2); padkeys[int(p[1])] = p[2]
    try: font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 11)
    except OSError: font = ImageFont.load_default()
    im = panel_im.copy(); dr = ImageDraw.Draw(im, "RGBA")
    def tag(x, y, text, col=(255, 214, 64)):
        w = dr.textlength(text, font=font)
        dr.rounded_rectangle((x - w / 2 - 3, y - 8, x + w / 2 + 3, y + 8), 3, fill=(0, 0, 0, 215), outline=col + (255,)); dr.text((x - w / 2, y - 7), text, font=font, fill=col)
    short = {"Left Shift": "L-Shift", "Return": "Enter", "PageUp": "PgUp", "PageDown": "PgDn", "Insert": "Ins", "Delete": "Del"}
    for (c, r), k in keys.items():
        nm = cellname.get((c, r))
        if nm in layout:
            x, y, w, h = layout[nm]; tag(x + w / 2, y + h + 9, short.get(k, k))
    for n, k in padkeys.items():
        x, y, w, h = pads[n]; tag(x + w / 2, y + h / 2, k.replace("Keypad ", "KP "), (130, 220, 255))
    dr.text((20, 838), "Yellow: keyboard key of a panel button.  Blue: numeric-keypad key of a pad.  [ ] or mouse wheel: DATA wheel.  Tab: fast-forward.  Ctrl+R: power cycle.  Esc: quit.",
            font=font, fill=(255, 255, 255, 255))
    im.save(os.path.join(OUT, "keymap.png")); print("wrote keymap.png")


if __name__ == "__main__":
    screens(); keymap(panel())
