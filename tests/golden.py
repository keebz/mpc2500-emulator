#!/usr/bin/env python3
"""Golden regression suite for the MPC2500 emulator.

The emulator is deterministic (all timing derives from the instruction counter), so the LCD contents and the captured
audio of a fixed scenario are reproducible bit for bit. Each scenario boots a firmware, drives it with a key/pad
script, and records a hash of the LCD frame (and of captured WAV audio). Refactors must not change any hash; a
deliberate behaviour fix updates the golden file with --update and says why in NOTES.md.

  python3 tests/golden.py [--bin emu/mpc2500emu] [--filter substr] [--update] [-j N]
"""
import argparse, concurrent.futures, hashlib, json, os, shutil, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "archive")
IMG = {
    "stock": os.path.join(FW, "v124", "mpc2500.bin"),
    "jv313": os.path.join(FW, "MPC2500", "mpc2500_jv313.bin"),
    "xl390": os.path.join(FW, "MPC2500", "mpc2500_xl390.osx"),
    "light100": os.path.join(FW, "MPC2500", "mpc2500_light100.osx"),
    "jv263": os.path.join(FW, "MPC2500", "mpc2500_jv263.os3"),
    "clear": os.path.join(FW, "clear", "mpc2500_v124_clear_internal.bin"),
}
KIT_PRELUDE = ("tap 0 3;wait 20000000;padhit 2 100;wait 30000000;tap 2 8;wait 30000000;tap 3 8;wait 1600000000;"
               "tap 0 2;wait 30000000;tap 0 3;wait 20000000;padhit 7 100;wait 30000000;tap 4 4;wait 20000000")


def scenarios():
    s = []
    s.append(dict(name="stock_boot", img="stock", boot=300_000_000, forks={"main": "wait 1000000;dump a"}))
    s.append(dict(name="stock_mode_pages", img="stock", boot=300_000_000,
                  forks={"p%d" % n: "tap 0 3;wait 20000000;padhit %d 100;wait 40000000;dump a" % n for n in range(1, 17)}))
    keys = {}
    for c in range(5):
        for r in range(11):
            keys["k%d_%d" % (c, r)] = "tap %d %d;wait 25000000;dump a" % (c, r)
    s.append(dict(name="stock_keys", img="stock", boot=300_000_000, forks=keys))
    # the DATA wheel must do exactly what the equivalent number of DATA +/- button presses does (Sq field, starts at 01)
    taps = lambda n: "tap 4 4;" * n
    s.append(dict(name="wheel_parity", img="stock", boot=300_000_000,
                  forks={"w1": "wheel 1 1;dump a", "b1": taps(1) + "dump a", "w3": "wheel 1 3;dump a", "b3": taps(3) + "dump a",
                         "w9": "wheel 1 9;dump a", "b9": taps(9) + "dump a", "w5d2": "wheel 1 5;wheel -1 2;dump a", "b3x": taps(3) + "dump a"},
                  same=[("w1_a.bin", "b1_a.bin"), ("w3_a.bin", "b3_a.bin"), ("w9_a.bin", "b9_a.bin"), ("w5d2_a.bin", "b3x_a.bin")]))
    s.append(dict(name="kit_audio", img="stock", boot=300_000_000, prelude=KIT_PRELUDE,
                  forks=dict([("p%d" % n, "audio 1;padhit %d 100;wait 80000000;audiosave a;dump a" % n) for n in (1, 5, 9, 14, 16)])))
    mixer = "tap 0 2;wait 20000000;press 0 3;padhit 8 100;release 0 3;wait 30000000;tap 3 3;wait 5000000;"       # MIXER page, pan knob of pad A01
    hit = "audio 1;padhit 1 100;wait 60000000;audiosave a"
    s.append(dict(name="pan_stereo", img="stock", boot=300_000_000, prelude=KIT_PRELUDE,
                  forks={"left": mixer + "wheel -1 60;" + hit, "right": mixer + "wheel 1 60;" + hit, "mid": mixer + hit}))
    s.append(dict(name="jv313_boot", img="jv313", boot=300_000_000, forks={"main": "wait 1000000;dump a"}))
    for k in ("xl390", "light100", "jv263"):
        s.append(dict(name=k + "_boot", img=k, boot=400_000_000, env={"FLASH_MODE": "full"}, forks={"main": "wait 1000000;dump a"}))
    s.append(dict(name="clear_boot", img="clear", boot=300_000_000, forks={"main": "wait 1000000;dump a"}))
    return s


_prepared = {}
def prepare_image(key, raw):
    """Legacy path: build the flash image with tools/mkflash.py (adds the boot checksum). With --raw the emulator's own loader does it."""
    if raw: return IMG[key]
    if key not in _prepared:
        dst = os.path.join(tempfile.gettempdir(), "gold_flash_%s.bin" % key)
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mkflash.py"), IMG[key], dst], check=True, stdout=subprocess.DEVNULL,
                       env=dict(os.environ, FLASH_MODE="full" if os.path.getsize(IMG[key]) > 0xEFFF0 else "safe"))
        _prepared[key] = dst
    return _prepared[key]


def run_scenario(sc, binary, card, raw=False):
    out = tempfile.mkdtemp(prefix="gold_" + sc["name"] + "_")
    env = dict(os.environ, SIOF_MODEL="1", SODIMM_MB="64", CARD=card)
    env.update(sc.get("env", {}))
    specs = []
    if sc.get("prelude"): specs.append("PRELUDE=" + sc["prelude"])
    for k, v in sc["forks"].items(): specs.append("%s=%s" % (k, v))
    t0 = time.time()
    p = subprocess.run([binary, prepare_image(sc["img"], raw), "--exp", out, str(sc["boot"])] + specs, env=env, cwd=os.path.dirname(binary),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=1500)
    res = {}
    for f in sorted(os.listdir(out)):
        path = os.path.join(out, f)
        if f.endswith(".bin") or f.endswith(".wav"):
            res[f] = hashlib.sha256(open(path, "rb").read()).hexdigest()[:16]
    fatal = [l for l in p.stdout.splitlines() if "FAULT REPORT" in l or "UNIMPLEMENTED" in l or "ILLEGAL" in l]
    for x, y in sc.get("same", []):
        if res.get(x) != res.get(y): fatal.append("PARITY FAILED: %s != %s" % (x, y))
    shutil.rmtree(out, ignore_errors=True)
    return sc["name"], res, fatal, p.returncode, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "emu", "mpc2500emu"))
    ap.add_argument("--filter", default="")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("-j", type=int, default=6)
    ap.add_argument("--raw", action="store_true", help="pass raw firmware files; the emulator's loader converts them")
    a = ap.parse_args(); a.bin = os.path.abspath(a.bin)
    for k in IMG: prepare_image(k, a.raw)        # build images up front (single thread) so scenarios can share them
    card = os.path.join(ROOT, "card", "card.img")
    gold_path = os.path.join(ROOT, "tests", "golden.json")
    gold = json.load(open(gold_path)) if os.path.exists(gold_path) else {}
    todo = [s for s in scenarios() if a.filter in s["name"]]
    bad = 0
    with concurrent.futures.ThreadPoolExecutor(a.j) as ex:
        for name, res, fatal, rc, dt in ex.map(lambda s: run_scenario(s, a.bin, card, a.raw), todo):
            if a.update:
                gold[name] = res; print("updated %-18s %3d outputs  %.0fs" % (name, len(res), dt)); continue
            exp = gold.get(name)
            if exp is None: print("NEW     %-18s (no golden; run with --update)" % name); bad += 1; continue
            diff = sorted(k for k in set(exp) | set(res) if exp.get(k) != res.get(k))
            status = "ok" if not diff and rc == 0 and not any("PARITY" in f for f in fatal) else "FAIL"
            if status == "FAIL": bad += 1
            print("%-7s %-18s %3d outputs  %.0fs%s%s" % (status, name, len(res), dt, ("  diff: " + ", ".join(diff[:8])) if diff else "", ("  rc=%d" % rc) if rc else ""))
            if fatal: print("        fatal markers:", fatal[:3])
    if a.update:
        json.dump(gold, open(gold_path, "w"), indent=1, sort_keys=True); print("wrote", gold_path)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
