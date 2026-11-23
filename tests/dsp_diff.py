#!/usr/bin/env python3
"""Differential test of the DSP instruction space (MOVS, MOVX/MOVY, parallel ALU/multiply) in emu/sh3.c + emu/dsp.c against the
independent model in dsp_ref.py (written from the Renesas manual).  Uses the same harness (cpu_diff.c) as cpu_diff.py.

  python3 tests/dsp_diff.py [--cases N] [--seed S] [--limit K]
Exit status 0 = no mismatch.  Only specified encodings are generated (no reserved codes, no operand contention, no out-of-range
shift amounts); see the header of dsp_ref.py for the unspecified cases.
"""
import argparse, collections, os, random, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import cpu_diff, dsp_ref
from cpu_diff import make_state, pack_case, unpack, diff_fields, OUT_WORDS, rnd32

DZ = [5, 7, 8, 9, 10, 11, 12, 14]
KINDS_COND = [0xB1, 0xA1, 0x95, 0xB5, 0xA5, 0x81, 0x91, 0x89, 0xA9, 0x99, 0xB9, 0x9D, 0xBD, 0x8D, 0xD9, 0xF9, 0xC9, 0xE9, 0xED, 0xFD, 0xCD, 0xDD]
KINDS_UNCOND = [0xA0, 0xB0, 0x84, 0x88, 0xA8, 0x98, 0xB8]


def gen_a(rng):
    """valid field A bits [9:0] (X and Y transfers); returns (bits, xload_dest or None, yload_dest or None)"""
    h = rng.getrandbits(2) << 8
    xd = rng.getrandbits(1); yd = rng.getrandbits(1)
    xk = rng.choice(["nop", "ld", "st"]); yk = rng.choice(["nop", "ld", "st"])
    xl = yl = None
    if xk != "nop":
        h |= xd << 7 | (1 << 5 if xk == "st" else 0) | rng.choice([1, 2, 3]) << 2
        if xk == "ld": xl = "x1" if xd else "x0"
    if yk != "nop":
        h |= yd << 6 | (1 << 4 if yk == "st" else 0) | rng.choice([1, 2, 3])
        if yk == "ld": yl = "y1" if yd else "y0"
    return h, xl, yl


def gen_b(rng, st):
    """valid field B word; returns (word, alu_dest_names) and may adjust the state (shift amounts)"""
    d = st["dsp"]; k = rng.random()
    xx, yy = rng.getrandbits(2), rng.getrandbits(2); zz = rng.choice(DZ)
    names = {5: "a1", 7: "a0", 8: "x0", 9: "x1", 10: "y0", 11: "y1", 12: "m0", 14: "m1"}
    if k < 0.08:                                                       # PSHL #imm / PSHA #imm
        if rng.random() < 0.5: n = rng.randrange(-16, 17); return (n & 63) << 4 | zz, [names[zz]]
        n = rng.randrange(-32, 33); return 0x1000 | (n & 127) << 4 | zz, [names[zz]]
    if k < 0.20:                                                       # PMULS (optionally + PADD/PSUB)
        top = rng.choice([4, 6, 7]); e, f = rng.getrandbits(2), rng.getrandbits(2)
        du, dg = rng.getrandbits(2), rng.getrandbits(2)
        w = top << 12 | e << 10 | f << 8 | xx << 6 | yy << 4 | dg << 2 | (du if top != 4 else 0)
        dests = [dsp_ref.DG[dg]] + ([dsp_ref.DU[du]] if top != 4 else [])
        if top != 4 and dsp_ref.DU[du] == dsp_ref.DG[dg]: return None, None
        return w, dests
    if rng.random() < 0.2: hi = rng.choice(KINDS_UNCOND)
    else:
        base = rng.choice(KINDS_COND); hi = (base & ~3) | rng.choice([1, 1, 2, 3])
    unsingle = hi & ~3 | 1
    if hi in (0x88, 0x98, 0x89, 0x99, 0x9D, 0xD9, 0xC9, 0xCD, 0xDD, 0xED, 0xFD, 0x8D): pass
    w = hi << 8 | xx << 6 | yy << 4 | zz
    base = (hi & ~3) | 1 if hi & 3 else hi
    # single-source forms: unused field bits are zero; X forms use xx only, Y forms yy only
    if base in (0x89, 0x99, 0x9D, 0xD9, 0xC9, 0x88, 0x98): w = hi << 8 | xx << 6 | zz
    if base in (0xA9, 0xB9, 0xBD, 0xF9, 0xE9, 0xA8, 0xB8): w = hi << 8 | yy << 4 | zz
    if base in (0x8D, 0xCD, 0xDD, 0xED, 0xFD): w = hi << 8 | zz
    if base == 0x84: w = hi << 8 | xx << 6 | yy << 4
    # valid shift amounts for the register-controlled shifts
    if base in (0x81, 0x91):
        lim = 16 if base == 0x81 else 32; bits = 63 if base == 0x81 else 127
        for r in ("y0", "y1", "m0", "m1"):
            n = rng.randrange(-lim, lim + 1)
            d[r] = ((n & bits) << 16 | rng.getrandbits(16)) & 0xFFFFFFFF
    dest = None if base == 0x84 else names[zz]
    return w, ([dest] if dest and base not in (0xED, 0xFD) else [])


def gen_case(rng):
    st = make_state(rng); st["sr"] |= 1 << 12 if rng.random() < 0.97 else 0
    if rng.random() < 0.3: st["sr"] &= ~((1 << 10) | (1 << 11))
    st["dsp"]["dsr"] = (st["dsp"]["dsr"] & 0xF1) | (rng.randrange(0, 6) << 1)      # CS in 0-5 (6,7 reserved)
    st["pc"] &= ~3
    kind = rng.random()
    if kind < 0.18:                                                    # MOVS
        ds = rng.choice([5, 7, 8, 9, 10, 11, 12, 13, 14, 15]); guard = ds in (13, 15)
        isl = 0 if guard else rng.getrandbits(1)
        st_ = 0 if guard else rng.getrandbits(1)
        op = 0xF400 | rng.getrandbits(2) << 8 | ds << 4 | rng.getrandbits(2) << 2 | isl << 1 | st_
        for i in (2, 3, 4, 5): st["r"][i] &= ~3 if rng.random() < 0.8 else 0xFFFFFFFF
        return st, [op, 0, 0, 0]
    ha, xl, yl = gen_a(rng)
    if kind < 0.30:                                                    # double data transfer
        op = 0xF000 | ha
        for i in (4, 5, 6, 7): st["r"][i] &= 0xFFFFFFFE if rng.random() < 0.8 else 0xFFFFFFFF
        return st, [op, 0, 0, 0]
    for _ in range(20):
        w, dests = gen_b(rng, st)
        if w is None: continue
        if (xl and xl in dests) or (yl and yl in dests): continue
        op = 0xF800 | ha
        return st, [op, w, 0, 0]
    return st, [0xF000, 0, 0, 0]


def ref_out(st, code, seed):
    r = dsp_ref.Dsp(st, code, seed)
    try: r.run()
    except dsp_ref.Unspecified as e: return {"unspec": str(e)}
    out = {"illegal": r.illegal, "reason": r.reason}
    if not r.illegal:
        out.update(r=r.r[:], bank=list(st["bank"]), pc=r.pc & 0xFFFFFFFF, pr=st["pr"], gbr=st["gbr"], vbr=st["vbr"], mach=r.d_mach & 0xFFFFFFFF,
                   macl=r.d_macl & 0xFFFFFFFF, sr=r.sr, ssr=st["ssr"], spc=st["spc"], tra=st["tra"], sleeping=0, expevt=0, writes=list(r.writes),
                   dsp={k: v & 0xFFFFFFFF for k, v in r.d.items()})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", type=int, default=200000); ap.add_argument("--seed", type=int, default=7); ap.add_argument("--limit", type=int, default=12)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="dspdiff_"); exe = os.path.join(tmp, "cpu_diff")
    subprocess.run(["gcc", "-O1", "-g", "-w", "-o", exe, os.path.join(HERE, "cpu_diff.c"), os.path.join(ROOT, "emu", "sh3.c"), os.path.join(ROOT, "emu", "dsp.c")], check=True)
    rng = random.Random(a.seed); cases, meta = [], []
    for _ in range(a.cases):
        st, code = gen_case(rng); seed = rng.getrandbits(32)
        code = code + [0] * (4 - len(code)); code[2] = rng.getrandbits(16); code[3] = rng.getrandbits(16)
        if code[0] & 0x800 == 0: code[1] = 0x0009                       # delay-free neighbour for non-parallel forms
        cases.append(pack_case(st, code, seed)); meta.append((st, code, seed))
    cin, cout = os.path.join(tmp, "in.bin"), os.path.join(tmp, "out.bin")
    open(cin, "wb").write(struct.pack("<I", len(cases)) + b"".join(cases))
    subprocess.run([exe, cin, cout], check=True)
    buf = open(cout, "rb").read()
    bad = collections.defaultdict(list); n_ok = n_unspec = 0
    for i, (st, code, seed) in enumerate(meta):
        ref = ref_out(st, code, seed)
        if "unspec" in ref: n_unspec += 1; continue
        c = unpack(buf, i * OUT_WORDS * 4)
        if ref["illegal"] != c["illegal"]:
            bad[(code[0] >> 8, (code[1] >> 8) if code[0] & 0x800 else -1)].append(("illegal ref=%d(%s) c=%d" % (ref["illegal"], ref["reason"], c["illegal"]), st, code)); continue
        if ref["illegal"]: n_ok += 1; continue
        d = diff_fields(ref, c)
        if d: bad[(code[0] >> 8, (code[1] >> 8) if code[0] & 0x800 else -1)].append((d, st, code))
        else: n_ok += 1
    print("cases %d  ok %d  unspecified-skipped %d  mismatching (header,alu) groups %d" % (len(meta), n_ok, n_unspec, len(bad)))
    shown = 0
    for key in sorted(bad, key=lambda k: -len(bad[k])):
        if shown >= a.limit: break
        d, st, code = bad[key][0]
        print("  hdr/alu hi %s: %d cases e.g. code %s sr=%08x dsr=%02x\n      %s" % (key, len(bad[key]), " ".join("%04x" % w for w in code[:2]), st["sr"], st["dsp"]["dsr"], d if isinstance(d, str) else "; ".join(d[:5])))
        shown += 1
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
