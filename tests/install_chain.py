#!/usr/bin/env python3
"""Virtual-unit install chain, run through the firmware's own installers (needs the firmware files in archive/):
  1. a new unit is created from the stock Akai OS 1.24 (the chooser's "Flash OS")
  2. JJOS JV 3.13 is installed as MPC2500.BIN from the stock OS's Load page (the Akai OS updater)
  3. JJOS XL 3.90 is installed from JV 3.13's Load page ("OS-XL update"); the stock OS must refuse it
  4. the unit powers on into OS-XL
Checks the flash contents byte for byte against the files. Usage: python3 tests/install_chain.py   (about 2 minutes)"""
import os, subprocess, sys, tempfile, shutil
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMU = os.path.join(ROOT, "emu", "mpc2500emu")
ARC = os.path.join(ROOT, "archive")
STOCK, JV, XL = (os.path.join(ARC, p) for p in ("v124/mpc2500.bin", "MPC2500/mpc2500_jv313.bin", "MPC2500/mpc2500_xl390.osx"))
PRE = "PRELUDE=tap 0 3;wait 20000000;padhit 2 100;wait 40000000"     # MODE + PAD 2 = Load page
ok = True; nrun = 0

def check(cond, what):
    global ok
    print(("PASS  " if cond else "FAIL  ") + what); ok &= bool(cond)

def make_card(tmp, name, src, dest):
    d = os.path.join(tmp, name + "_files"); os.makedirs(d); shutil.copy(src, os.path.join(d, dest))
    img = os.path.join(tmp, name + ".img")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mkfat16.py"), img, "64", os.path.join(d, dest)], check=True, stdout=subprocess.DEVNULL)
    return img

def run(unit, card, script, tmp, flash=None):
    env = dict(os.environ, SIOF_MODEL="1", SODIMM_MB="64", CARD=card)
    global nrun; nrun += 1; out = os.path.join(tmp, "run%d" % nrun)
    args = [EMU, "--unit", unit] + (["--flash", flash] if flash else []) + ["--exp", out, "400000000" if not flash else "300000000", PRE, "a=" + script]
    return subprocess.run(args, env=env, cwd=os.path.dirname(EMU), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=900).stdout, out

def header(path, off=0x10000): return open(path, "rb").read()[off:off + 24]

tmp = tempfile.mkdtemp(prefix="install_chain_")
unit = os.path.join(tmp, "unit.flash")
try:
    # the stock OS must refuse the XL image ("File too big")
    xlcard = make_card(tmp, "xl_as_bin", XL, "MPC2500.BIN")
    so, _ = run(unit, xlcard, "tap 2 8;wait 30000000;tap 3 8;wait 300000000;flashsave %s.s0" % unit, tmp, flash=STOCK)
    st = open(STOCK, "rb").read(); d0 = open(unit + ".s0", "rb").read()
    check(d0[0x10000:0xEFFF0] == open(unit, "rb").read()[0x10000:0xEFFF0] and d0[0x10000:len(st)] == st[0x10000:] and d0[0x100000:0x180000] == b"\xff" * 0x80000,
          "stock OS refuses the XL image named MPC2500.BIN: OS and internal area unchanged")
    check(open(unit, "rb").read()[0x10000:0x10020].startswith(b"MPC2500         Version=1.24"), "unit created from the stock OS 1.24")
    jvcard = make_card(tmp, "jv", JV, "MPC2500.BIN")
    run(unit, jvcard, "tap 2 8;wait 30000000;tap 3 8;wait 1500000000;flashsave %s.s1" % unit, tmp)
    d = open(unit + ".s1", "rb").read(); jv = open(JV, "rb").read()
    check(d[0x10000:len(jv)] == jv[0x10000:], "JV 3.13 installed by the stock OS updater: flash matches the file")
    shutil.copy(unit + ".s1", unit)
    xl2 = make_card(tmp, "xl", XL, "XL390.OSX")
    run(unit, xl2, "tap 2 8;wait 30000000;tap 3 8;wait 3500000000;flashsave %s.s2" % unit, tmp)
    d = open(unit + ".s2", "rb").read(); xl = open(XL, "rb").read()
    check(d[0x10000:len(xl)] == xl[0x10000:], "OS-XL 3.90 installed from JV 3.13's Load page: flash matches the file")
    shutil.copy(unit + ".s2", unit)
    _, o = run(unit, xl2, "wait 1000000;dump a", tmp)
    lcd = open(os.path.join(o, "a_a.bin"), "rb").read()
    check(any(lcd), "unit powers on into OS-XL (display is drawn)")
finally:
    shutil.rmtree(tmp, ignore_errors=True)
print("ALL PASSED" if ok else "FAILED"); sys.exit(0 if ok else 1)
