#!/usr/bin/env python3
"""Build a CompactFlash-style disk image: MBR + one FAT16 partition, files in the root (8.3 names).
Written from scratch (no mtools/root needed).  usage: mkfat16.py out.img SIZE_MB file [file ...] [--dir NAME]"""
import struct, sys, os, time

def build(out, size_mb, files, label='MPC2500'):
    SPC = 8                      # sectors per cluster (4 KiB)
    total = size_mb * 2048       # 512-byte sectors in the whole disk
    start = 63                   # partition start LBA (classic CHS-era layout)
    part = total - start
    reserved, nfats, rootent = 1, 2, 512
    rootsec = rootent * 32 // 512
    # FAT size: iterate
    fatsz = 1
    while True:
        data = part - reserved - nfats * fatsz - rootsec
        clusters = data // SPC
        need = ((clusters + 2) * 2 + 511) // 512
        if need <= fatsz: break
        fatsz = need
    assert 4085 <= clusters < 65525, clusters
    fat = bytearray(fatsz * 512); fat[0:4] = b'\xf8\xff\xff\xff'
    img = bytearray()   # we build only the used prefix and pad later
    root = bytearray(rootent * 32)
    nextc = 2; datasecs = {}     # cluster -> bytes
    ents = []
    def alloc(nbytes):
        nonlocal nextc
        n = max(1, (nbytes + SPC * 512 - 1) // (SPC * 512)); first = nextc
        for i in range(n):
            c = first + i; fat[c * 2:c * 2 + 2] = struct.pack('<H', c + 1 if i < n - 1 else 0xFFFF)
        nextc += n; return first, n
    t = time.localtime(); dt = ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday; tm = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    def name83(fn):
        b, _, e = fn.upper().rpartition('.') if '.' in fn else (fn.upper(), '', '')
        return (b[:8].ljust(8) + e[:3].ljust(3)).encode('ascii')
    payload = []
    for path in files:
        d = open(path, 'rb').read(); c, n = alloc(len(d)); payload.append((c, d))
        ents.append(name83(os.path.basename(path)) + struct.pack('<BBBHHHHHHHL', 0x20, 0, 0, tm, dt, dt, 0, tm, dt, c, len(d)))
    assert len(ents) <= rootent
    for i, e in enumerate(ents): root[i * 32:i * 32 + 32] = e
    # boot sector
    bs = bytearray(512)
    bs[0:3] = b'\xeb\x3c\x90'; bs[3:11] = b'MSDOS5.0'
    struct.pack_into('<HBHBHHBHHHLL', bs, 11, 512, SPC, reserved, nfats, rootent, 0 if part >= 65536 else part, 0xF8, fatsz, 63, 16, start, part if part >= 65536 else 0)
    bs[36] = 0x80; bs[38] = 0x29; struct.pack_into('<L', bs, 39, 0x4D504332); bs[43:54] = label.ljust(11).encode(); bs[54:62] = b'FAT16   '; bs[510:512] = b'\x55\xaa'
    mbr = bytearray(512)
    def chs(lba):
        c, h, s = lba // (16 * 63), (lba // 63) % 16, lba % 63 + 1
        return bytes([h, ((c >> 2) & 0xC0) | s, c & 0xFF])
    mbr[446:462] = bytes([0x80]) + chs(start) + bytes([0x06]) + chs(total - 1) + struct.pack('<LL', start, part)
    mbr[510:512] = b'\x55\xaa'
    with open(out, 'wb') as f:
        f.truncate(total * 512)
        f.seek(0); f.write(mbr)
        base = start * 512
        f.seek(base); f.write(bs)
        for k in range(nfats):
            f.seek(base + (reserved + k * fatsz) * 512); f.write(fat)
        f.seek(base + (reserved + nfats * fatsz) * 512); f.write(root)
        dstart = base + (reserved + nfats * fatsz + rootsec) * 512
        for c, d in payload:
            f.seek(dstart + (c - 2) * SPC * 512); f.write(d)
    return dict(total_sectors=total, part_start=start, clusters=clusters, files=len(ents))

if __name__ == '__main__':
    out, mb = sys.argv[1], int(sys.argv[2]); fl = sys.argv[3:]
    print(build(out, mb, fl))
