#!/usr/bin/env python3
"""Validate an MPC2500 OS build and write the update files.
usage: make_os.py build.bin outdir [--name NAME]
Outputs outdir/MPC2500.BIN (copy of the build: the file the running OS's LOAD-page 'OS update' accepts; flash layout), outdir/NAME.bin (canonical build artifact, flash layout: boot block 0-0xFFFF + OS from 0x10000, same form as Akai's mpc2500.bin) and
outdir/MPC2500.SOS = the OS part only (build[0x10000:]): the boot updater writes file byte 0 to flash 0x10000, so the SOS must start at the
'MPC2500 ... Version=' header (verified by installing in the emulator: the full .bin renamed to .SOS leaves flash with the boot vectors at 0x10000
and the OS does not start). Checks (SAFE mode, always): size <= 0xEFFF0, header 'MPC2500' at 0x10000 (not a TEST/run-in-place image), no data at or
beyond 0xEFFF0 (the stock updater only writes flash sectors 1-14 = 0x10000-0xEFFFF), boot block 0-0xFFFF identical to stock 1.24
(the updater never rewrites it, so any difference would silently not install)."""
import sys, os, hashlib, shutil
here = os.path.dirname(os.path.abspath(__file__))
stock = open(os.path.join(here, '..', 'archive', 'v124', 'mpc2500.bin'), 'rb').read()
src, out = sys.argv[1], sys.argv[2]
name = sys.argv[sys.argv.index('--name') + 1] if '--name' in sys.argv else os.path.splitext(os.path.basename(src))[0]
d = open(src, 'rb').read()
errs, warns = [], []
if len(d) > 0xEFFF0: errs.append('image is %d bytes; the OS window ends at 0xEFFF0 = 983,024 bytes' % len(d))
if d[0x10000:0x10007] != b'MPC2500': errs.append("no 'MPC2500' header at 0x10000")
if d[0x10000:0x1000C] == b'MPC2500 TEST': errs.append("'MPC2500 TEST' header: run-in-place image, not an updater image")
if d[:0x10000] != stock[:0x10000]: errs.append('boot block (0-0xFFFF) differs from stock; the updater does not rewrite it, so the change would not install')
if any(b != 0xFF for b in d[0xEFFF0:]) and len(d) > 0xEFFF0: errs.append('data at/after 0xEFFF0')
if errs:
    print('REFUSED:'); [print('  -', e) for e in errs]; sys.exit(1)
os.makedirs(out, exist_ok=True)
b = os.path.join(out, name + '.bin'); s = os.path.join(out, 'MPC2500.SOS'); m = os.path.join(out, 'MPC2500.BIN')
open(b, 'wb').write(d); open(m, 'wb').write(d); open(s, 'wb').write(d[0x10000:])
print('    NORMAL INSTALL: copy', m, 'to the card root; LOAD page -> select it -> DO IT (the running OS writes flash). Recovery: MPC2500.SOS via the boot updater.')
print('OK  %s  %d bytes (flash layout)' % (b, len(d)))
print('    SOS %d bytes, free in OS window: %d bytes (max 917,488)  sha256(SOS) %s' % (len(d) - 0x10000, 0xEFFF0 - len(d), hashlib.sha256(d[0x10000:]).hexdigest()[:16]))
print('    copy to the card root as MPC2500.SOS:', s)
