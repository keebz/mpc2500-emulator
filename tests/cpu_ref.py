"""Reference model of the SH3 / SH3-DSP *integer* instruction set, written from the Renesas manual (REJ09B0317-0400,
section 8.2 "Instruction Descriptions" operation pseudocode) and used only to test emu/sh3.c. It is deliberately simple
and independent of the C core.

Where the manual is ambiguous or self-contradictory the choice is recorded in AMBIGUITIES below (and in NOTES.md):
the test treats those as "either behaviour accepted" only when listed.
"""
M32 = 0xFFFFFFFF

SR_T, SR_S, SR_Q, SR_M = 1, 2, 0x100, 0x200
SR_BL, SR_RB, SR_MD = 1 << 28, 1 << 29, 1 << 30
SR_DSP = 1 << 12

AMBIGUITIES = {
    "SHAD/SHLD with negative Rm whose low 5 bits are 0": "pseudocode shifts by 0, text says 1..32 -> modelled as shift by 32 (text)",
    "LDS to DSR": "SW manual pseudocode masks to 4 bits, SH7727 manual defines an 8-bit DSR (GT Z N V CS DC) -> 8 bits",
    "MACH width": "SH-3 pseudocode keeps MACH as 10 bits sign-extended, the SH3-DSP note says MACH is a full 32-bit register -> full 32 bit",
}


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s8(v):
    v &= 0xFF
    return v - 0x100 if v & 0x80 else v


def hash8(seed, addr):
    h = ((addr & M32) * 2654435761 & M32) ^ ((seed * 40503) & M32)
    h ^= h >> 15
    h = (h * 2246822519) & M32
    h ^= h >> 13
    h = (h * 3266489917) & M32
    h ^= h >> 16
    return h & 0xFF


# illegal-slot instructions (manual 6.x Delay_Slot description + LDC Rm,SR note)
def is_slot_illegal(op):
    hi = op >> 12
    if hi in (0xA, 0xB): return True                                   # BRA, BSR
    if op & 0xFF00 in (0x8900, 0x8B00, 0x8D00, 0x8F00): return True    # BT BF BT/S BF/S
    if op & 0xFF00 == 0xC300: return True                              # TRAPA
    if op in (0x000B, 0x002B): return True                             # RTS RTE
    if op & 0xF0FF in (0x0003, 0x0023, 0x402B, 0x400B): return True    # BSRF BRAF JMP JSR
    if op & 0xF0FF in (0x400E, 0x4007): return True                    # LDC Rm,SR / LDC.L @Rm+,SR
    return False


class Ref:
    """One CPU instruction (plus its delay slot) from an explicit state."""

    def __init__(self, st, code, seed):
        self.r = list(st["r"]); self.bank = list(st["bank"])
        self.pc = st["pc"]; self.pr = st["pr"]; self.gbr = st["gbr"]; self.vbr = st["vbr"]
        self.mach = st["mach"]; self.macl = st["macl"]; self.sr = st["sr"]; self.ssr = st["ssr"]; self.spc = st["spc"]
        self.tra = st["tra"]; self.expevt = 0
        self.dsp = dict(st["dsp"])           # a0 a1 x0 x1 y0 y1 m0 m1 dsr mod rs re a0g a1g
        self.code = code; self.seed = seed; self.base_pc = st["pc"]
        self.writes = []; self.illegal = 0; self.sleeping = 0; self.reason = ""

    # ---- memory ----
    def rd(self, addr, size):
        addr &= M32
        if size == 2 and not addr & 1 and self.base_pc <= addr < self.base_pc + 8:
            return self.code[(addr - self.base_pc) >> 1]
        v = 0
        for i in range(size):
            v |= hash8(self.seed, (addr + i) & M32) << (8 * i)
        return v

    def wr(self, addr, val, size):
        if len(self.writes) < 8:
            self.writes.append(((addr & M32), size, val & ((1 << (8 * size)) - 1)))

    # ---- state helpers ----
    @property
    def T(self): return self.sr & 1

    def setT(self, b): self.sr = (self.sr & ~1) | (1 if b else 0)

    def set_sr(self, v):
        v &= 0x7FFF1FFF            # bit 31 and bits 15:13 are reserved (read as 0)
        if (v ^ self.sr) & SR_RB:
            self.r[0:8], self.bank = self.bank[:], self.r[0:8]
        self.sr = v

    def privileged_ok(self):
        if self.sr & SR_MD: return True
        self.illegal = 1; self.reason = self.reason or "priv"
        return False

    def need_dsp(self):
        """DSP-class instruction: illegal when SR.DSP = 0 (SH7727 manual 2.2.1, 'DSP bit')."""
        if self.sr & SR_DSP: return True
        self.illegal = 1; self.reason = self.reason or "dsp0"
        return False

    # ---- execution ----
    def run(self):
        op = self.code[0]
        self.exec(op, slot=False)
        return self

    def delay(self, target):
        """Execute the delay-slot instruction (at pc+2) with full effects, then branch."""
        sop = self.code[1]
        if is_slot_illegal(sop):
            self.illegal = 1; self.reason = self.reason or "slot"
            return
        self.pc = (self.base_pc + 2) & M32          # the slot instruction's own address
        self.exec(sop, slot=True)
        if not self.illegal:
            self.pc = target & M32

    def exec(self, op, slot):
        R = self.r
        n = (op >> 8) & 15; m = (op >> 4) & 15; d = op & 15; i = op & 255
        hi = op >> 12
        pc = self.pc            # address of this instruction
        nxt = (pc + 2) & M32
        self.pc = nxt           # default: fall through (branches override)
        T = self.T

        def ill():
            self.illegal = 1; self.reason = self.reason or "decode"

        if hi == 0x0:
            lo = op & 15
            if lo == 2:
                reg = m
                if reg & 8:
                    if not self.privileged_ok(): return
                    R[n] = self.bank[reg & 7]; return
                names = {0: "sr", 1: "gbr", 2: "vbr", 3: "ssr", 4: "spc"}
                if reg in names:
                    if names[reg] != "gbr" and not self.privileged_ok(): return
                    R[n] = getattr(self, names[reg]) & M32; return
                if reg in (5, 6, 7):
                    if not self.need_dsp(): return
                    if not self.privileged_ok(): return
                    R[n] = self.dsp[{5: "mod", 6: "rs", 7: "re"}[reg]] & M32; return
                return ill()
            if lo == 3:
                if m == 0:      # BSRF
                    self.pr = (pc + 4) & M32; return self.delay(pc + 4 + R[n])
                if m == 2:      # BRAF
                    return self.delay(pc + 4 + R[n])
                if m == 8: return          # PREF
                return ill()
            if lo in (4, 5, 6):
                sz = {4: 1, 5: 2, 6: 4}[lo]
                self.wr(R[0] + R[n], R[m], sz); return
            if lo == 7: self.macl = (R[n] * R[m]) & M32; return
            if lo == 8:
                if n != 0: return ill()
                if m == 0: self.setT(0); return
                if m == 1: self.setT(1); return
                if m == 2: self.mach = self.macl = 0; return
                if m == 3:
                    if not self.privileged_ok(): return
                    return
                if m == 4: self.sr &= ~SR_S; return
                if m == 5: self.sr |= SR_S; return
                return ill()
            if lo == 9:
                if m == 0 and n == 0: return
                if m == 1 and n == 0: self.sr &= ~(SR_M | SR_Q | SR_T); return
                if m == 2: R[n] = T; return
                return ill()
            if lo == 10:
                if m in (0, 1, 2) and n >= 0:
                    R[n] = {0: self.mach, 1: self.macl, 2: self.pr}[m] & M32; return
                dsp_names = {6: "dsr", 7: "a0", 8: "x0", 9: "x1", 10: "y0", 11: "y1"}
                if m in dsp_names:
                    if not self.need_dsp(): return
                    R[n] = self.dsp[dsp_names[m]] & M32; return
                return ill()
            if lo == 11:
                if op == 0x000B: return self.delay(self.pr)                 # RTS
                if op == 0x001B:
                    if not self.privileged_ok(): return
                    self.sleeping = 1; return
                if op == 0x002B:
                    if not self.privileged_ok(): return
                    # RTE: the delay slot runs with the SR restored by RTE
                    tgt, newsr = self.spc, self.ssr
                    self.set_sr(newsr)
                    return self.delay(tgt)
                return ill()
            if lo in (12, 13, 14):
                a = (R[0] + R[m]) & M32
                R[n] = s8(self.rd(a, 1)) & M32 if lo == 12 else s16(self.rd(a, 2)) & M32 if lo == 13 else self.rd(a, 4)
                return
            if lo == 15:   # MAC.L @Rm+,@Rn+
                return self.mac_l(m, n)
            return ill()

        if hi == 0x1:
            self.wr(R[n] + d * 4, R[m], 4); return

        if hi == 0x2:
            if d == 0: self.wr(R[n], R[m], 1); return
            if d == 1: self.wr(R[n], R[m], 2); return
            if d == 2: self.wr(R[n], R[m], 4); return
            if d in (4, 5, 6):
                sz = {4: 1, 5: 2, 6: 4}[d]
                self.wr(R[n] - sz, R[m], sz); R[n] = (R[n] - sz) & M32; return
            if d == 7:
                q = (R[n] >> 31) & 1; mm = (R[m] >> 31) & 1
                self.sr = (self.sr & ~(SR_Q | SR_M)) | (SR_Q if q else 0) | (SR_M if mm else 0); self.setT(q != mm); return
            if d == 8: self.setT((R[n] & R[m]) == 0); return
            if d == 9: R[n] &= R[m]; return
            if d == 10: R[n] ^= R[m]; return
            if d == 11: R[n] |= R[m]; return
            if d == 12:
                x = R[n] ^ R[m]
                self.setT(any(((x >> s) & 0xFF) == 0 for s in (0, 8, 16, 24))); return
            if d == 13: R[n] = (((R[m] & 0xFFFF) << 16) | (R[n] >> 16)) & M32; return
            if d == 14: self.macl = ((R[n] & 0xFFFF) * (R[m] & 0xFFFF)) & M32; return
            if d == 15: self.macl = (s16(R[n]) * s16(R[m])) & M32; return
            return ill()

        if hi == 0x3:
            if d == 0: self.setT(R[n] == R[m]); return
            if d == 2: self.setT(R[n] >= R[m]); return
            if d == 3: self.setT(s32(R[n]) >= s32(R[m])); return
            if d == 4: return self.div1(m, n)
            if d == 5:
                p = R[n] * R[m]; self.mach = (p >> 32) & M32; self.macl = p & M32; return
            if d == 6: self.setT(R[n] > R[m]); return
            if d == 7: self.setT(s32(R[n]) > s32(R[m])); return
            if d == 8: R[n] = (R[n] - R[m]) & M32; return
            if d == 10:
                tmp1 = (R[n] - R[m]) & M32; tmp0 = R[n]
                R[n] = (tmp1 - T) & M32
                t = 1 if tmp0 < tmp1 else 0
                if tmp1 < R[n]: t = 1
                self.setT(t); return
            if d == 11:
                r = s32(R[n]) - s32(R[m]); R[n] = r & M32; self.setT(r > 0x7FFFFFFF or r < -0x80000000); return
            if d == 12: R[n] = (R[n] + R[m]) & M32; return
            if d == 13:
                p = s32(R[n]) * s32(R[m]); self.mach = (p >> 32) & M32; self.macl = p & M32; return
            if d == 14:
                tmp1 = (R[n] + R[m]) & M32; tmp0 = R[n]
                R[n] = (tmp1 + T) & M32
                t = 1 if tmp0 > tmp1 else 0
                if tmp1 > R[n]: t = 1
                self.setT(t); return
            if d == 15:
                r = s32(R[n]) + s32(R[m]); R[n] = r & M32; self.setT(r > 0x7FFFFFFF or r < -0x80000000); return
            return ill()

        if hi == 0x4:
            return self.op4(op, n, m, d)

        if hi == 0x5:
            R[n] = self.rd(R[m] + d * 4, 4); return

        if hi == 0x6:
            if d == 0: R[n] = s8(self.rd(R[m], 1)) & M32; return
            if d == 1: R[n] = s16(self.rd(R[m], 2)) & M32; return
            if d == 2: R[n] = self.rd(R[m], 4); return
            if d == 3: R[n] = R[m]; return
            if d in (4, 5, 6):
                sz = {4: 1, 5: 2, 6: 4}[d]
                v = self.rd(R[m], sz); v = s8(v) if sz == 1 else s16(v) if sz == 2 else v
                if n != m: R[m] = (R[m] + sz) & M32
                R[n] = v & M32; return
            if d == 7: R[n] = ~R[m] & M32; return
            if d == 8: R[n] = (R[m] & 0xFFFF0000) | ((R[m] & 0xFF) << 8) | ((R[m] >> 8) & 0xFF); return
            if d == 9: R[n] = ((R[m] << 16) | (R[m] >> 16)) & M32; return
            if d == 10:
                tmp = (0 - R[m]) & M32; R[n] = (tmp - T) & M32
                t = 1 if 0 < tmp else 0
                if tmp < R[n]: t = 1
                self.setT(t); return
            if d == 11: R[n] = (0 - R[m]) & M32; return
            if d == 12: R[n] = R[m] & 0xFF; return
            if d == 13: R[n] = R[m] & 0xFFFF; return
            if d == 14: R[n] = s8(R[m]) & M32; return
            if d == 15: R[n] = s16(R[m]) & M32; return

        if hi == 0x7:
            R[n] = (R[n] + s8(i)) & M32; return

        if hi == 0x8:
            sub = n
            if sub == 0: self.wr(R[m] + d, R[0], 1); return
            if sub == 1: self.wr(R[m] + d * 2, R[0], 2); return
            if sub == 2:
                if not self.need_dsp(): return
                self.dsp_setrc_imm(i); return
            if sub == 4: R[0] = s8(self.rd(R[m] + d, 1)) & M32; return
            if sub == 5: R[0] = s16(self.rd(R[m] + d * 2, 2)) & M32; return
            if sub == 8: self.setT(R[0] == (s8(i) & M32)); return
            if sub in (9, 11, 13, 15):
                cond_true = (T == 1) if sub in (9, 13) else (T == 0)
                tgt = (pc + 4 + s8(i) * 2) & M32
                if sub in (9, 11):
                    if cond_true: self.pc = tgt
                else:
                    if cond_true: self.delay(tgt)
                return
            if sub == 12:
                if not self.need_dsp(): return
                self.dsp["rs"] = (pc + 4 + s8(i) * 2) & M32; return
            if sub == 14:
                if not self.need_dsp(): return
                self.dsp["re"] = (pc + 4 + s8(i) * 2) & M32; return
            return ill()

        if hi == 0x9:
            R[n] = s16(self.rd((pc + 4 + i * 2) & M32, 2)) & M32; return

        if hi == 0xA:
            disp = (op & 0xFFF) - 0x1000 if op & 0x800 else op & 0xFFF
            return self.delay(pc + 4 + disp * 2)
        if hi == 0xB:
            disp = (op & 0xFFF) - 0x1000 if op & 0x800 else op & 0xFFF
            self.pr = (pc + 4) & M32
            return self.delay(pc + 4 + disp * 2)

        if hi == 0xC:
            sub = n
            if sub == 0: self.wr(self.gbr + i, R[0], 1); return
            if sub == 1: self.wr(self.gbr + i * 2, R[0], 2); return
            if sub == 2: self.wr(self.gbr + i * 4, R[0], 4); return
            if sub == 3:     # TRAPA
                self.spc = nxt; self.ssr = self.sr; self.tra = i << 2; self.expevt = 0x160
                self.set_sr(self.sr | SR_MD | SR_BL | SR_RB)
                self.pc = (self.vbr + 0x100) & M32; return
            if sub == 4: R[0] = s8(self.rd(self.gbr + i, 1)) & M32; return
            if sub == 5: R[0] = s16(self.rd(self.gbr + i * 2, 2)) & M32; return
            if sub == 6: R[0] = self.rd(self.gbr + i * 4, 4); return
            if sub == 7: R[0] = (((pc + 4) & ~3) + i * 4) & M32; return
            if sub == 8: self.setT((R[0] & i) == 0); return
            if sub == 9: R[0] &= i; return
            if sub == 10: R[0] ^= i; return
            if sub == 11: R[0] |= i; return
            a = (self.gbr + R[0]) & M32
            if sub == 12: self.setT((self.rd(a, 1) & i) == 0); return
            if sub == 13: self.wr(a, self.rd(a, 1) & i, 1); return
            if sub == 14: self.wr(a, self.rd(a, 1) ^ i, 1); return
            if sub == 15: self.wr(a, self.rd(a, 1) | i, 1); return

        if hi == 0xD:
            R[n] = self.rd(((pc + 4) & ~3) + i * 4, 4); return

        if hi == 0xE:
            R[n] = s8(i) & M32; return

        return ill()     # 0xF: DSP/FPU space, tested separately

    # ---- helpers ----
    def dsp_setrc_imm(self, i):
        self.sr = (self.sr & ~0x0FFF0000) | ((i & 0xFF) << 16)

    def div1(self, m, n):
        R = self.r
        old_q = 1 if self.sr & SR_Q else 0
        Mb = 1 if self.sr & SR_M else 0
        q = (R[n] >> 31) & 1
        R[n] = ((R[n] << 1) | self.T) & M32
        tmp0 = R[n]
        if old_q == 0:
            if Mb == 0:
                R[n] = (R[n] - R[m]) & M32; tmp1 = 1 if R[n] > tmp0 else 0
                q = tmp1 if q == 0 else (1 if tmp1 == 0 else 0)
            else:
                R[n] = (R[n] + R[m]) & M32; tmp1 = 1 if R[n] < tmp0 else 0
                q = (1 if tmp1 == 0 else 0) if q == 0 else tmp1
        else:
            if Mb == 0:
                R[n] = (R[n] + R[m]) & M32; tmp1 = 1 if R[n] < tmp0 else 0
                q = tmp1 if q == 0 else (1 if tmp1 == 0 else 0)
            else:
                R[n] = (R[n] - R[m]) & M32; tmp1 = 1 if R[n] > tmp0 else 0
                q = (1 if tmp1 == 0 else 0) if q == 0 else tmp1
        self.sr = (self.sr & ~SR_Q) | (SR_Q if q else 0)
        self.setT(q == Mb)

    def mac_l(self, m, n):
        R = self.r
        tn = s32(self.rd(R[n], 4)); R[n] = (R[n] + 4) & M32
        tm = s32(self.rd(R[m], 4)); R[m] = (R[m] + 4) & M32
        prod = tn * tm
        mac = (self.mach << 32) | self.macl
        if self.sr & SR_S:
            # 48-bit saturating accumulate: only the low 16 bits of MACH take part (manual pseudocode)
            acc = s48(((self.mach & 0xFFFF) << 32) | self.macl) + prod          # 48-bit signed view of MAC
            if acc > 0x7FFFFFFFFFFF: acc = 0x7FFFFFFFFFFF
            if acc < -0x800000000000: acc = -0x800000000000
            self.mach = (acc >> 32) & M32; self.macl = acc & M32
        else:
            acc = ((mac + prod) & 0xFFFFFFFFFFFFFFFF)
            self.mach = (acc >> 32) & M32; self.macl = acc & M32

    def mac_w(self, m, n):
        R = self.r
        tn = s16(self.rd(R[n], 2)); R[n] = (R[n] + 2) & M32
        tm = s16(self.rd(R[m], 2)); R[m] = (R[m] + 2) & M32
        prod = tn * tm
        if self.sr & SR_S:
            r = s32(self.macl) + prod
            if r > 0x7FFFFFFF: self.mach |= 1; r = 0x7FFFFFFF
            elif r < -0x80000000: self.mach |= 1; r = -0x80000000
            self.macl = r & M32
        else:
            acc = (((self.mach << 32) | self.macl) + prod) & 0xFFFFFFFFFFFFFFFF
            self.mach = (acc >> 32) & M32; self.macl = acc & M32

    def op4(self, op, n, m, d):
        R = self.r
        T = self.T
        low = op & 0xFF
        if d == 15: return self.mac_w(m, n)
        if d == 12:                      # SHAD
            sh = R[m]
            if not sh & 0x80000000: R[n] = (R[n] << (sh & 31)) & M32
            else:
                cnt = 32 - (sh & 31) if (sh & 31) else 32
                R[n] = ((s32(R[n]) >> cnt) & M32) if cnt < 32 else (M32 if R[n] & 0x80000000 else 0)
            return
        if d == 13:                      # SHLD
            sh = R[m]
            if not sh & 0x80000000: R[n] = (R[n] << (sh & 31)) & M32
            else:
                cnt = 32 - (sh & 31) if (sh & 31) else 32
                R[n] = (R[n] >> cnt) & M32 if cnt < 32 else 0
            return
        tbl = {
            0x00: lambda: (self.setT(R[n] >> 31), R.__setitem__(n, (R[n] << 1) & M32)),            # SHLL
            0x01: lambda: (self.setT(R[n] & 1), R.__setitem__(n, R[n] >> 1)),                      # SHLR
            0x04: lambda: (self.setT(R[n] >> 31), R.__setitem__(n, ((R[n] << 1) | (R[n] >> 31)) & M32)),   # ROTL
            0x05: lambda: (self.setT(R[n] & 1), R.__setitem__(n, ((R[n] >> 1) | ((R[n] & 1) << 31)) & M32)),  # ROTR
            0x08: lambda: R.__setitem__(n, (R[n] << 2) & M32), 0x09: lambda: R.__setitem__(n, R[n] >> 2),
            0x18: lambda: R.__setitem__(n, (R[n] << 8) & M32), 0x19: lambda: R.__setitem__(n, R[n] >> 8),
            0x28: lambda: R.__setitem__(n, (R[n] << 16) & M32), 0x29: lambda: R.__setitem__(n, R[n] >> 16),
            0x20: lambda: (self.setT(R[n] >> 31), R.__setitem__(n, (R[n] << 1) & M32)),            # SHAL
            0x21: lambda: (self.setT(R[n] & 1), R.__setitem__(n, (R[n] >> 1) | (R[n] & 0x80000000))),  # SHAR
        }
        if low in tbl: tbl[low](); return
        if low == 0x24:   # ROTCL
            t = T; self.setT(R[n] >> 31); R[n] = ((R[n] << 1) | t) & M32; return
        if low == 0x25:   # ROTCR
            t = T; self.setT(R[n] & 1); R[n] = ((R[n] >> 1) | (t << 31)) & M32; return
        if low == 0x10: R[n] = (R[n] - 1) & M32; self.setT(R[n] == 0); return           # DT
        if low == 0x11: self.setT(s32(R[n]) >= 0); return
        if low == 0x15: self.setT(s32(R[n]) > 0); return
        if low == 0x1B:   # TAS.B
            v = self.rd(R[n], 1); self.setT(v == 0); self.wr(R[n], v | 0x80, 1); return
        if low == 0x2B:   # JMP
            return self.delay(R[n])
        if low == 0x0B:   # JSR
            self.pr = (self.pc + 2) & M32       # self.pc was advanced to pc+2 on entry; PR = instruction address + 4
            return self.delay(R[n])
        if low == 0x14:   # SETRC Rm
            if not self.need_dsp(): return
            self.sr = (self.sr & ~0x0FFF0000) | ((R[n] & 0xFFF) << 16); return
        # ---- control / system register loads and stores ----
        grp = (low >> 4)
        lsb = low & 15
        ctl = {0: "sr", 1: "gbr", 2: "vbr", 3: "ssr", 4: "spc", 5: "mod", 6: "rs", 7: "re"}
        if lsb == 0x0E:       # LDC Rm,<ctl> / Rm_BANK
            if grp & 8:
                if not self.privileged_ok(): return
                self.bank[grp & 7] = R[n]; return
            if grp > 7: return self.err()
            name = ctl[grp]
            if grp >= 5 and not self.need_dsp(): return
            if name != "gbr" and not self.privileged_ok(): return
            if name == "sr": return self.set_sr(R[n])
            if name in ("gbr", "vbr", "ssr", "spc"): setattr(self, name, R[n]); return
            self.dsp[name] = R[n]; return
        if lsb == 0x07:       # LDC.L @Rm+,<ctl>
            if grp & 8:
                if not self.privileged_ok(): return
                v = self.rd(R[n], 4); self.bank[grp & 7] = v; R[n] = (R[n] + 4) & M32; return
            name = ctl[grp]
            if grp >= 5 and not self.need_dsp(): return
            if name != "gbr" and not self.privileged_ok(): return
            v = self.rd(R[n], 4); R[n] = (R[n] + 4) & M32
            if name == "sr": return self.set_sr(v)
            if name in ("gbr", "vbr", "ssr", "spc"): setattr(self, name, v); return
            self.dsp[name] = v; return
        if lsb == 0x03:       # STC.L <ctl>,@-Rn
            if grp & 8:
                if not self.privileged_ok(): return
                R[n] = (R[n] - 4) & M32; self.wr(R[n], self.bank[grp & 7], 4); return
            name = ctl[grp]
            if grp >= 5 and not self.need_dsp(): return
            if name != "gbr" and not self.privileged_ok(): return
            R[n] = (R[n] - 4) & M32
            v = self.sr if name == "sr" else getattr(self, name) if name in ("gbr", "vbr", "ssr", "spc") else self.dsp[name]
            self.wr(R[n], v, 4); return
        sysr = {0: "mach", 1: "macl", 2: "pr"}
        dspsys = {6: "dsr", 7: "a0", 8: "x0", 9: "x1", 10: "y0", 11: "y1"}
        if lsb == 0x0A:       # LDS Rm,<sys>
            if grp in sysr: setattr(self, sysr[grp], R[n]); return
            if grp in dspsys:
                if not self.need_dsp(): return
                return self.lds_dsp(dspsys[grp], R[n])
            return self.err()
        if lsb == 0x06:       # LDS.L @Rm+,<sys>
            if grp in sysr: setattr(self, sysr[grp], self.rd(R[n], 4)); R[n] = (R[n] + 4) & M32; return
            if grp in dspsys:
                if not self.need_dsp(): return
                v = self.rd(R[n], 4); R[n] = (R[n] + 4) & M32; return self.lds_dsp(dspsys[grp], v)
            return self.err()
        if lsb == 0x02:       # STS.L <sys>,@-Rn
            if grp in sysr: R[n] = (R[n] - 4) & M32; self.wr(R[n], getattr(self, sysr[grp]), 4); return
            if grp in dspsys:
                if not self.need_dsp(): return
                R[n] = (R[n] - 4) & M32; self.wr(R[n], self.dsp[dspsys[grp]], 4); return
            return self.err()
        return self.err()

    def err(self):
        self.illegal = 1; self.reason = self.reason or "decode"

    def lds_dsp(self, name, v):
        v &= M32
        if name == "dsr": self.dsp["dsr"] = v & 0xFF; return
        self.dsp[name] = v
        if name == "a0": self.dsp["a0g"] = 0xFF if v & 0x80000000 else 0


def s48(v):
    v &= (1 << 48) - 1
    return v - (1 << 48) if v & (1 << 47) else v


def legal_opcodes_from_manual(json_path):
    """Set of all 16-bit opcodes the manual lists (section 8.2 patterns + TRAPA), for the decode-legality check."""
    import json
    pats = {p for _, _, p, _ in json.load(open(json_path))}
    pats.add("11000011iiiiiiii")        # TRAPA (row missed by the extractor)
    pats.add("11001110iiiiiiii")        # XOR.B #imm,@(R0,GBR) (row missed by the extractor)
    pats.add("0100nnnn01110010")        # STS.L A0,@-Rn: the manual prints the DSR code (01100010) twice - typo
    ok = set()
    for p in pats:
        mask = int("".join("1" if c in "01" else "0" for c in p), 2)
        val = int("".join(c if c in "01" else "0" for c in p), 2)
        for op in range(0x10000):
            if op & mask == val: ok.add(op)
    return ok
