#!/usr/bin/env python3
"""Polyphony check on the stock OS (needs archive/v124/mpc2500.bin and the kit card): a mono voice (pad 1) and a stereo voice (pad 3) played together must
come out as the exact sum of the two played alone (the audio engine adds voices into a block buffer that a 32-word repeat loop clears; a repeat-loop bug once left
stale data there and produced a buzz). Also checks that nothing is silent/garbled with three voices.   usage: python3 tests/mix_check.py [--bin emu/mpc2500emu_hl]"""
import argparse, os, shutil, struct, subprocess, sys, tempfile, wave
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRE = ("tap 0 3;wait 20000000;padhit 2 100;wait 30000000;tap 2 8;wait 30000000;tap 3 8;wait 1600000000;tap 0 2;wait 30000000;tap 0 3;wait 20000000;"
       "padhit 7 100;wait 30000000;tap 4 4;wait 20000000")
hit = lambda pads: "audio 1;" + "".join("pad %d 100;" % p for p in pads) + "wait 4000000;" + "".join("padup %d;" % p for p in pads) + "wait 60000000;audiosave a"
ap = argparse.ArgumentParser(); ap.add_argument("--bin", default=os.path.join(ROOT, "emu", "mpc2500emu_hl")); a = ap.parse_args()
out = tempfile.mkdtemp(prefix="mixcheck_")
env = dict(os.environ, CARD=os.path.join(ROOT, "card", "test_card.img"), SODIMM_MB="64")
subprocess.run([a.bin, os.path.join(ROOT, "archive", "v124", "mpc2500.bin"), "--audio-shift", "4", "--exp", out, "300000000", "PRELUDE=" + PRE,
                "p1=" + hit([1]), "p3=" + hit([3]), "p5=" + hit([5]), "p13=" + hit([1, 3]), "p135=" + hit([1, 3, 5])], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
def ld(n):
    w = wave.open(os.path.join(out, n + "_a.wav")); k = w.getnframes(); return struct.unpack("<%dh" % (k * 2), w.readframes(k))[0::2]
p1, p3, p5, p13, p135 = (ld(n) for n in ("p1", "p3", "p5", "p13", "p135"))
def fit(t, part, lags, lo=4000, n=800):
    return min((sum(abs(t[i] - part[i + l]) for i in range(lo, lo + n) if 0 <= i + l < len(part)), l) for l in lags)[1]
def shifted(part, l): return [part[i + l] if 0 <= i + l < len(part) else 0 for i in range(len(p13))]
rel = lambda x, t: sum(abs(v) for v in x[3000:30000]) / max(1, sum(abs(v) for v in t[3000:30000]))
ok = True
l3 = fit([p13[i] - p1[i] for i in range(len(p13))], p3, range(-300, 301))
r = [p13[i] - p1[i] - shifted(p3, l3)[i] for i in range(len(p13))]
e2 = rel(r, p13); print("p1+p3 (mono+stereo): residual %.5f (voice offset %d samples)" % (e2, l3)); ok &= e2 < 0.01
t = [p135[i] - p5[i] for i in range(len(p135))]
l1 = fit(t, p1, range(-300, 301)); t2 = [t[i] - shifted(p1, l1)[i] for i in range(len(t))]
l3b = fit(t2, p3, range(-300, 301)); r3 = [t2[i] - shifted(p3, l3b)[i] for i in range(len(t))]
e3 = rel(r3, p135); print("p5+p1+p3 (three voices): residual %.5f" % e3); ok &= e3 < 0.01
z = p135[3000:40000].count(0) / 37000.0; print("silent samples with three voices: %.1f %%" % (100 * z)); ok &= z < 0.10
shutil.rmtree(out, ignore_errors=True)
print("PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)
