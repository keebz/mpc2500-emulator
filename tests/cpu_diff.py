#!/usr/bin/env python3
"""Differential test: emu/sh3.c vs the independent reference model in cpu_ref.py, over the whole non-DSP opcode space.

  python3 tests/cpu_diff.py [--states N] [--seed S] [--limit K]

For every 16-bit opcode 0x0000-0xEFFF (0xF000-0xFFFF is the DSP/FPU space, tested by dsp_diff.py) the harness runs
N random machine states through both implementations and compares the complete resulting state, the memory writes
and the illegal-instruction decision. Decode legality is also checked against the opcode patterns extracted from the
manual (tests/ref/manual_cpu_patterns.json).
"""
import argparse, collections, json, os, random, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import cpu_ref

IN_WORDS = 33 + 1 + 4 + 1 + 14
OUT_WORDS = 33 + 1 + 14 + 4 + 24
DSP_NAMES = ["a0", "a1", "x0", "x1", "y0", "y1", "m0", "m1", "dsr", "mod", "rs", "re", "a0g", "a1g"]
SPECIAL = [0, 1, 2, 3, 4, 7, 8, 15, 16, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFE0, 0x1F, 0x20]


def rnd32(rng):
    k = rng.random()
    if k < 0.25: return rng.choice(SPECIAL)
    if k < 0.40: return (-rng.randrange(1, 64)) & 0xFFFFFFFF
    if k < 0.55: return rng.randrange(0, 256)
    return rng.getrandbits(32)


SIMPLE_SLOTS = [0x0009, 0x7001, 0x6103, 0x2126, 0x3128, 0x4100, 0x0129, 0xE155, 0x6212, 0x2A44, 0x4118, 0x6004]


def make_state(rng):
    st = {"r": [rnd32(rng) for _ in range(16)], "bank": [rnd32(rng) for _ in range(8)]}
    st["pc"] = (rng.randrange(0x1000, 0xFFFF0000) & ~1) & 0xFFFFFFFF
    for k in ("pr", "gbr", "vbr", "mach", "macl", "ssr", "spc"): st[k] = rnd32(rng)
    sr = rng.getrandbits(4) | (rng.getrandbits(4) << 4)                # T S ... I3-I0 (bits 3:2 = RF, left random)
    sr |= rng.getrandbits(2) << 8                                        # Q M
    sr |= rng.getrandbits(1) << 10 | rng.getrandbits(1) << 11 | rng.getrandbits(1) << 12   # DMX DMY DSP
    sr |= (1 << 30 if rng.random() < 0.8 else 0) | rng.getrandbits(1) << 29 | rng.getrandbits(1) << 28   # MD RB BL
    st["sr"] = sr
    st["tra"] = 0
    st["dsp"] = {k: rnd32(rng) for k in DSP_NAMES}
    st["dsp"]["a0g"] &= 0xFF; st["dsp"]["a1g"] &= 0xFF; st["dsp"]["dsr"] &= 0xFF
    return st


def make_case(rng, op):
    st = make_state(rng)
    k = rng.random()
    op1 = rng.choice(SIMPLE_SLOTS) if k < 0.5 else rng.getrandbits(16) if k < 0.8 else rng.choice([0xA001, 0x8B01, 0x000B, 0x002B, 0x400E, 0x4007, 0xC301, 0x4B0B])
    if op1 >> 12 == 0xF: op1 &= 0x0FFF          # DSP-space slots are covered by dsp_diff.py
    code = [op, op1, rng.getrandbits(16), rng.getrandbits(16)]
    return st, code, rng.getrandbits(32)


def pack_case(st, code, seed):
    w = list(st["r"]) + list(st["bank"]) + [st["pc"], st["pr"], st["gbr"], st["vbr"], st["mach"], st["macl"], st["sr"], st["ssr"], st["spc"], st["tra"]]
    w += list(code) + [seed] + [st["dsp"][k] for k in DSP_NAMES]
    assert len(w) == IN_WORDS
    return struct.pack("<%dI" % IN_WORDS, *w)


def ref_result(st, code, seed):
    r = cpu_ref.Ref(st, code, seed).run()
    out = {"illegal": r.illegal, "reason": r.reason}
    if not r.illegal:
        out.update(r=r.r[:], bank=r.bank[:], pc=r.pc & 0xFFFFFFFF, pr=r.pr & 0xFFFFFFFF, gbr=r.gbr & 0xFFFFFFFF, vbr=r.vbr & 0xFFFFFFFF,
                   mach=r.mach & 0xFFFFFFFF, macl=r.macl & 0xFFFFFFFF, sr=r.sr & 0xFFFFFFFF, ssr=r.ssr & 0xFFFFFFFF, spc=r.spc & 0xFFFFFFFF,
                   tra=r.tra, sleeping=r.sleeping, expevt=r.expevt, writes=list(r.writes),
                   dsp={k: (v & 0xFFFFFFFF) for k, v in r.dsp.items()})
    return out


def unpack(buf, off):
    w = struct.unpack_from("<%dI" % OUT_WORDS, buf, off)
    out = {"illegal": w[48] != 0}
    out.update(r=list(w[0:16]), bank=list(w[16:24]), pc=w[24], pr=w[25], gbr=w[26], vbr=w[27], mach=w[28], macl=w[29], sr=w[30], ssr=w[31], spc=w[32],
               tra=w[33], sleeping=w[49], expevt=w[51])
    out["dsp"] = dict(zip(DSP_NAMES, w[34:48]))
    out["writes"] = [(w[52 + 3 * i], w[53 + 3 * i], w[54 + 3 * i]) for i in range(w[50])]
    return out


def diff_fields(a, b):
    bad = []
    for k in ("pc", "pr", "gbr", "vbr", "mach", "macl", "sr", "ssr", "spc", "tra", "sleeping", "expevt"):
        if a[k] != b[k]: bad.append("%s ref=%08x c=%08x" % (k, a[k], b[k]))
    for i in range(16):
        if a["r"][i] != b["r"][i]: bad.append("r%d ref=%08x c=%08x" % (i, a["r"][i], b["r"][i]))
    for i in range(8):
        if a["bank"][i] != b["bank"][i]: bad.append("bank%d ref=%08x c=%08x" % (i, a["bank"][i], b["bank"][i]))
    if a["writes"] != b["writes"]: bad.append("writes ref=%s c=%s" % (a["writes"], b["writes"]))
    for k in DSP_NAMES:
        if a["dsp"][k] != b["dsp"][k]: bad.append("dsp.%s ref=%08x c=%08x" % (k, a["dsp"][k], b["dsp"][k]))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--states", type=int, default=6)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--limit", type=int, default=3, help="examples to print per mismatching opcode family")
    ap.add_argument("--max-op", type=int, default=0xEFFF)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="cpudiff_")
    exe = os.path.join(tmp, "cpu_diff")
    subprocess.run(["gcc", "-O1", "-g", "-w", "-o", exe, os.path.join(HERE, "cpu_diff.c"), os.path.join(ROOT, "emu", "sh3.c"), os.path.join(ROOT, "emu", "dsp.c")], check=True)
    legal = cpu_ref.legal_opcodes_from_manual(os.path.join(HERE, "ref", "manual_cpu_patterns.json"))
    rng = random.Random(a.seed)
    cases, meta = [], []
    for op in range(0, a.max_op + 1):
        for _ in range(a.states):
            st, code, seed = make_case(rng, op)
            cases.append(pack_case(st, code, seed)); meta.append((op, st, code, seed))
    cin, cout = os.path.join(tmp, "in.bin"), os.path.join(tmp, "out.bin")
    open(cin, "wb").write(struct.pack("<I", len(cases)) + b"".join(cases))
    subprocess.run([exe, cin, cout], check=True)
    buf = open(cout, "rb").read()
    assert len(buf) == len(cases) * OUT_WORDS * 4, "harness output size mismatch"
    fam_bad = collections.defaultdict(list)
    legality = collections.defaultdict(lambda: [0, 0, 0])      # op -> [ref illegal, c illegal, n]
    for idx, (op, st, code, seed) in enumerate(meta):
        ref = ref_result(st, code, seed)
        c = unpack(buf, idx * OUT_WORDS * 4)
        # legality of the opcode itself (slot ops excluded: run without a delay slot influence by looking at op0 only)
        if ref["illegal"] != c["illegal"]:
            fam_bad[op].append(("ILLEGAL ref=%d(%s) c=%d slot=%04x sr=%08x" % (ref["illegal"], ref["reason"], c["illegal"], code[1], st["sr"]), st, code))
            continue
        if ref["illegal"]: continue
        d = diff_fields(ref, c)
        if d: fam_bad[op].append((d, st, code))
    # decode legality vs the manual's pattern list, using a state with MD=1 and a harmless slot
    ill_vs_manual = []
    for op in range(0, a.max_op + 1):
        st = make_state(random.Random(op)); st["sr"] |= cpu_ref.SR_MD | cpu_ref.SR_DSP
        code = [op, 0x0009, 0, 0]
        refill = ref_result(st, code, 1)["illegal"] != 0
        if refill != (op not in legal): ill_vs_manual.append((op, refill, op in legal))
    ncases = len(cases)
    nbad_ops = len(fam_bad)
    print("cases %d, opcodes with a C-vs-reference mismatch: %d" % (ncases, nbad_ops))
    shown = 0
    by_hi = collections.Counter(op >> 12 for op in fam_bad)
    print("mismatching opcodes by top nibble:", dict(sorted(by_hi.items())))
    for op in sorted(fam_bad):
        if shown >= a.limit * 12: break
        ex = fam_bad[op][0]
        print("  op %04x (%d/%d states)  %s" % (op, len(fam_bad[op]), a.states, ex[0] if isinstance(ex[0], str) else "; ".join(ex[0][:4])))
        shown += 1
    print("reference decode vs manual pattern list: %d disagreements" % len(ill_vs_manual))
    for op, refill, inman in ill_vs_manual[:20]:
        print("   op %04x ref_illegal=%s in_manual=%s" % (op, refill, inman))
    json.dump({"bad_ops": sorted(fam_bad)}, open(os.path.join(tmp, "bad.json"), "w"))
    print("work dir", tmp)
    sys.exit(1 if nbad_ops or ill_vs_manual else 0)


if __name__ == "__main__":
    main()
