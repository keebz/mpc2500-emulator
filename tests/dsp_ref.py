"""Reference model of the SH3-DSP instruction space 0xF000-0xFBFF (MOVS single transfers, MOVX/MOVY double transfers and the 32-bit
parallel-processing instructions), written from the Renesas SH3-DSP software manual (REJ09B0317-0400) independently of emu/dsp.c.

Manual sections used: 5.1-5.9 (operation functions, DC/CS semantics, saturation), 5.10-5.11 (transfers, contention), 6.3 (addressing,
modulo), 6.5 (encodings, Table 6.10-6.13) and 7.4 (opcode tables 7.21-7.29).

Manual inconsistencies / unspecified behaviour (the generator in dsp_diff.py avoids the unspecified cases, the model records the choices):
  * Tables 7.27/7.28 print the immediate-shift encodings swapped; Table 6.13 (and GNU binutils) have PSHL #imm = 00000, PSHA #imm = 00010.
  * The carry/borrow DC for PABS/PNEG/PINC/PDEC is not defined by the manual; the model uses the carry/borrow of the corresponding
    32-bit add/subtract (PNEG/PABS: 0 - x, PINC: x + H'10000, PDEC: x - H'10000), PRND uses the carry of x + H'8000 (stated in 5.7.3).
  * PSHA left-shift overflow: the 40-bit result wraps, then overflow = result outside the 32-bit range.
  * PRND with saturation: the sum is saturated first, then the low word is cleared (7FFF0000).
  * Contention (5.11), reserved codes, shift amounts outside the valid range and stores of A0G/A1G are never generated.
"""
import cpu_ref
from cpu_ref import M32, hash8

M40 = (1 << 40) - 1
SR_S, SR_DMX, SR_DMY, SR_DSP = 1 << 1, 1 << 10, 1 << 11, 1 << 12
REG = {5: "a1", 7: "a0", 8: "x0", 9: "x1", 10: "y0", 11: "y1", 12: "m0", 14: "m1"}      # Dz / Ds codes (Table 6.11, 6.13)
SX = ["x0", "x1", "a0", "a1"]; SY = ["y0", "y1", "m0", "m1"]
SE = ["x0", "x1", "y0", "a1"]; SF = ["y0", "y1", "x0", "a1"]; DG = ["m0", "m1", "a0", "a1"]; DU = ["x0", "y0", "a0", "a1"]
XMEM, YMEM = 0xA5000000, 0xA5010000


def s40(v):
    v &= M40
    return v - (1 << 40) if v >> 39 else v


def s32(v):
    v &= M32
    return v - (1 << 32) if v >> 31 else v


def s16(v):
    v &= 0xFFFF
    return v - (1 << 16) if v >> 15 else v


class Dsp:
    def __init__(self, st, code, seed):
        self.r = list(st["r"]); self.sr = st["sr"]; self.pc = st["pc"]
        self.d = dict(st["dsp"]); self.seed = seed; self.code = code; self.base = st["pc"]
        self.writes = []; self.illegal = 0; self.reason = ""
        self.st = st
        self.d_mach = st["mach"]; self.d_macl = st["macl"]

    # ---- memory / registers ----
    def rd16(self, a):
        a &= M32
        if not a & 1 and self.base <= a < self.base + 8: return self.code[(a - self.base) >> 1]
        return hash8(self.seed, a) | (hash8(self.seed, (a + 1) & M32) << 8)

    def rd32(self, a):
        return sum(hash8(self.seed, (a + i) & M32) << (8 * i) for i in range(4))

    def wr(self, a, v, size):
        if len(self.writes) < 8: self.writes.append((a & M32, size, v & ((1 << (8 * size)) - 1)))

    def get40(self, n):
        if n == "a0": return s40((self.d["a0g"] << 32) | self.d["a0"])
        if n == "a1": return s40((self.d["a1g"] << 32) | self.d["a1"])
        return s32(self.d[n])

    def put40(self, n, v):
        v &= M40
        self.d[n] = v & M32
        if n in ("a0", "a1"): self.d[n + "g"] = (v >> 32) & 0xFF

    def dsr(self): return self.d["dsr"]
    @property
    def DC(self): return self.d["dsr"] & 1
    def cs(self): return (self.d["dsr"] >> 1) & 7
    def sat(self): return bool(self.sr & SR_S)

    def set_flags(self, N, Z, V, GT, dc):
        self.d["dsr"] = (self.d["dsr"] & ~0xF1) | (dc & 1) | (V << 4) | (N << 5) | (Z << 6) | (GT << 7)

    def dc_arith(self, N, Z, V, carry, shift=False):
        m = self.cs()
        if m == 0: return carry
        if m == 1: return N
        if m == 2: return Z
        if m == 3: return V
        if m == 4: return 0 if shift else int(not ((N ^ V) or Z))
        if m == 5: return 0 if shift else int(not (N ^ V))
        return 0

    def dc_logic(self, N, Z, shout=0):
        m = self.cs()
        return {0: shout, 1: N, 2: Z}.get(m, 0)

    # ---- ALU result writers ----
    def arith(self, dz, val, carry, upd, shift=False, bound_hi=0x7FFFFFFF, bound_lo=-0x80000000, lowclear=False):
        v = s40(val)
        ov = int(v > bound_hi or v < bound_lo)
        if ov and self.sat():
            v = bound_hi if v > 0 else bound_lo
            ov = 0
        if lowclear: v = s40(v & ~0xFFFF)
        if dz: self.put40(dz, v)
        if upd:
            N = int(v < 0); Z = int(v == 0); GT = 0 if shift else int(not ((N ^ ov) or Z))
            self.set_flags(N, Z, ov, GT, self.dc_arith(N, Z, ov, carry, shift))

    def logic(self, dz, r16, upd, shout=0):
        self.put40(dz, r16 << 16)
        if upd:
            N = r16 >> 15; Z = int(r16 == 0)
            self.set_flags(N, Z, 0, 0, self.dc_logic(N, Z, shout))

    # ---- main entry ----
    def run(self):
        op = self.code[0]
        if not self.sr & SR_DSP:
            self.illegal = 1; self.reason = "dsp0"; return self
        self.pc = (self.pc + 2) & M32
        if op == 0xF000: return self
        if 0xF400 <= op <= 0xF7FF: self.movs(op); return self
        if op & 0xF400 == 0xF000:           # F000-F3FF double transfer, F800-FBFF parallel
            alu = None
            if op & 0x800:
                alu = self.code[1]; self.pc = (self.pc + 2) & M32
            self.parallel(op, alu)
            return self
        self.illegal = 1; self.reason = "decode"
        return self

    # ---- MOVS ----
    def movs(self, op):
        reg = [4, 5, 2, 3][(op >> 8) & 3]
        ds = REG[(op >> 4) & 15] if ((op >> 4) & 15) in REG else None
        guard = {13: "a1g", 15: "a0g"}.get((op >> 4) & 15)
        mode = (op >> 2) & 3; isl = (op >> 1) & 1; st = op & 1
        size = 4 if isl else 2
        R = self.r
        if mode == 0: R[reg] = (R[reg] - size) & M32
        a = R[reg]
        if st:
            v = self.get_ds(ds, guard) & M32
            if isl: self.wr(a, v, 4)
            else: self.wr(a, (v >> 16) & 0xFFFF, 2)
        else:
            if isl: self.set_ds(ds, guard, self.rd32(a), True)
            else: self.set_ds(ds, guard, self.rd16(a), False)
        if mode == 2: R[reg] = (R[reg] + size) & M32
        elif mode == 3: R[reg] = (R[reg] + R[8]) & M32

    def get_ds(self, ds, guard):
        return self.get40(ds) & M32

    def set_ds(self, ds, guard, val, long):
        if guard:
            self.d[guard] = val & 0xFF; return
        if long: v = s32(val)
        else: v = s32((val & 0xFFFF) << 16)
        self.put40(ds, v)           # accumulators sign-extend into the guard bits; the other registers keep the low 32 bits

    # ---- parallel / double transfers ----
    def parallel(self, h, alu):
        R = self.r
        xr = 4 + ((h >> 9) & 1); yr = 6 + ((h >> 8) & 1)
        xm = (h >> 2) & 3; xst = (h >> 5) & 1; xd = (h >> 7) & 1
        ym = h & 3; yst = (h >> 4) & 1; yd = (h >> 6) & 1
        xaddr = XMEM | (R[xr] & 0xFFFE); yaddr = YMEM | (R[yr] & 0xFFFE)
        xdata = (self.get40("a1" if xd else "a0") >> 16) & 0xFFFF       # stores use the pre-ALU register value
        ydata = (self.get40("a1" if yd else "a0") >> 16) & 0xFFFF
        if alu is not None: self.alu(alu)
        dmx, dmy = bool(self.sr & SR_DMX), bool(self.sr & SR_DMY)
        me, ms = (self.d["mod"] >> 16) & 0xFFFF, self.d["mod"] & 0xFFFF
        if xm or xst:           # NOPX is mode 0 / store 0
            if xst: self.wr(xaddr, xdata, 2)
            else: self.d["x1" if xd else "x0"] = (self.rd16(xaddr) << 16) & M32
            inc = {1: 0, 2: 2, 3: R[8]}[xm]
            if dmx and not dmy and xm >= 2 and (R[xr] & 0xFFFF) == me: R[xr] = (R[xr] & 0xFFFF0000) | ms
            else: R[xr] = (R[xr] + inc) & M32
        if ym or yst:
            if yst: self.wr(yaddr, ydata, 2)
            else: self.d["y1" if yd else "y0"] = (self.rd16(yaddr) << 16) & M32
            inc = {1: 0, 2: 2, 3: R[9]}[ym]
            if dmy and ym >= 2 and (R[yr] & 0xFFFF) == me: R[yr] = (R[yr] & 0xFFFF0000) | ms
            else: R[yr] = (R[yr] + inc) & M32

    # ---- field B ----
    def alu(self, f):
        hi = f >> 8
        xx, yy, zz = (f >> 6) & 3, (f >> 4) & 3, f & 15
        # immediate shifts
        if f & 0xE800 == 0:
            dz = REG[zz]; x = self.get40(dz)
            if f & 0x1000:                                          # PSHA #imm7
                n = ((f >> 4) & 127); n = n - 128 if n & 64 else n
                self.shift_arith(dz, x, n, True)
            else:                                                   # PSHL #imm6
                n = ((f >> 4) & 63); n = n - 64 if n & 32 else n
                self.shift_logic(dz, x, n, True)
            return
        # PMULS family
        top = f >> 12
        if top in (4, 6, 7):
            se = self.get40(SE[(f >> 10) & 3]); sf = self.get40(SF[(f >> 8) & 3])
            x = self.get40(SX[xx]); y = self.get40(SY[yy])
            if top == 6 or top == 7:
                du = DU[f & 3]
                if top == 7: self.add_sub(du, x, y, False, True, 1)
                else: self.add_sub(du, x, y, True, True, 1)
            p = s16(se >> 16) * s16(sf >> 16) * 2
            if p == 0x80000000: p = 0x7FFFFFFF if self.sat() else -0x80000000
            self.put40(DG[(f >> 2) & 3], p)
            return
        cond = hi & 3
        kind = ALU_KIND.get(hi) if cond == 0 else ALU_KIND.get((hi & ~3) | 1)
        if kind is None: raise Unspecified("alu word %04x" % f)
        if cond == 2 and not self.DC: return
        if cond == 3 and self.DC: return
        upd = cond in (0, 1)
        x = self.get40(SX[xx]); y = self.get40(SY[yy]); dz = REG.get(zz)
        if kind == "psubc": self.addsubc(dz, x, y, False)
        elif kind == "paddc": self.addsubc(dz, x, y, True)
        elif kind == "pcmp": self.add_sub(None, x, y, True, True, 1)
        elif kind in ("pabsx", "pabsy"):
            s = x if kind == "pabsx" else y
            self.arith(dz, -s if s < 0 else s, int(s < 0 and (s & M32) != 0), True)
        elif kind in ("prndx", "prndy"):
            s = x if kind == "prndx" else y
            self.arith(dz, s + 0x8000, ((s & M32) + 0x8000) >> 32, True, lowclear=True)
        elif kind == "padd": self.add_sub(dz, x, y, False, upd, 1)
        elif kind == "psub": self.add_sub(dz, x, y, True, upd, 1)
        elif kind == "pand": self.logic(dz, ((x >> 16) & 0xFFFF) & ((y >> 16) & 0xFFFF), upd)
        elif kind == "pxor": self.logic(dz, ((x >> 16) & 0xFFFF) ^ ((y >> 16) & 0xFFFF), upd)
        elif kind == "por": self.logic(dz, ((x >> 16) & 0xFFFF) | ((y >> 16) & 0xFFFF), upd)
        elif kind == "pshl": self.shift_logic(dz, x, s_amt(y, 6), upd)
        elif kind == "psha": self.shift_arith(dz, x, s_amt(y, 7), upd)
        elif kind in ("pdecx", "pdecy", "pincx", "pincy"):
            s = x if kind.endswith("x") else y
            if kind.startswith("pdec"): v = ((s >> 16) - 1) << 16; c = int((s & M32) < 0x10000)
            else: v = ((s >> 16) + 1) << 16; c = ((s & M32) + 0x10000) >> 32
            self.arith(dz, v, c, upd, bound_hi=0x7FFF0000, bound_lo=-0x80000000)
        elif kind in ("pdmsbx", "pdmsby"):
            s = x if kind.endswith("x") else y
            yv = ~s if s < 0 else s
            k = max([b for b in range(39) if (yv >> b) & 1], default=-1)
            r = 30 - k
            self.put40(dz, r << 16)
            if upd:
                N = int(r < 0); Z = int(r == 0); GT = int(r > 0)
                dc = {1: N, 2: Z, 4: GT, 5: int(not N)}.get(self.cs(), 0)
                self.set_flags(N, Z, 0, GT, dc)
        elif kind == "pclr": self.arith(dz, 0, 0, upd)
        elif kind in ("pcopyx", "pcopyy"): self.arith(dz, x if kind == "pcopyx" else y, 0, upd)
        elif kind in ("pnegx", "pnegy"):
            s = x if kind == "pnegx" else y
            self.arith(dz, -s, int((s & M32) != 0), upd)
        elif kind == "pldsh": self.d_mach = self.get40(dz) & M32
        elif kind == "pldsl": self.d_macl = self.get40(dz) & M32
        elif kind == "pstsh": self.put40(dz, s32(self.d_mach))
        elif kind == "pstsl": self.put40(dz, s32(self.d_macl))

    def add_sub(self, dz, x, y, sub, upd, _):
        if sub: carry = int((x & M32) < (y & M32)); val = x - y
        else: carry = int(((x & M32) + (y & M32)) >> 32); val = x + y
        self.arith(dz, val, carry, upd)

    def addsubc(self, dz, x, y, add):
        dc = self.DC
        if add:
            val = x + y + dc; carry = int(((x & M32) + (y & M32) + dc) >> 32)
        else:
            val = x - y - dc; carry = int((x & M32) < (y & M32) + dc)
        v = s40(val); ov = int(v > 0x7FFFFFFF or v < -0x80000000)
        if ov and self.sat(): v = 0x7FFFFFFF if v > 0 else -0x80000000; ov = 0
        self.put40(dz, v)
        N = int(v < 0); Z = int(v == 0)
        self.set_flags(N, Z, ov, int(not ((N ^ ov) or Z)), carry)           # DC = carry/borrow whatever CS says

    def shift_arith(self, dz, x, n, upd):
        if n >= 0:
            r = s40(x << n); shout = (x >> (40 - n)) & 1 if n else 0
        else:
            k = -n; r = x >> k; shout = (x >> (k - 1)) & 1
        self.arith(dz, r, shout, upd, shift=True)

    def shift_logic(self, dz, x, n, upd):
        t = (x >> 16) & 0xFFFF
        if n >= 0:
            r = (t << n) & 0xFFFF; shout = (t >> (16 - n)) & 1 if n else 0
        else:
            k = -n; r = t >> k; shout = (t >> (k - 1)) & 1
        self.logic(dz, r, upd, shout)


def s_amt(y, bits):
    n = (y >> 16) & ((1 << bits) - 1)
    return n - (1 << bits) if n >> (bits - 1) else n


ALU_KIND = {0xA0: "psubc", 0xB0: "paddc", 0x84: "pcmp", 0x88: "pabsx", 0xA8: "pabsy", 0x98: "prndx", 0xB8: "prndy",
            0xB1: "padd", 0xA1: "psub", 0x95: "pand", 0xB5: "por", 0xA5: "pxor", 0x81: "pshl", 0x91: "psha",
            0x89: "pdecx", 0xA9: "pdecy", 0x99: "pincx", 0xB9: "pincy", 0x9D: "pdmsbx", 0xBD: "pdmsby",
            0x8D: "pclr", 0xD9: "pcopyx", 0xF9: "pcopyy", 0xC9: "pnegx", 0xE9: "pnegy",
            0xED: "pldsh", 0xFD: "pldsl", 0xCD: "pstsh", 0xDD: "pstsl"}


class Unspecified(Exception):
    pass
