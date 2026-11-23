#!/usr/bin/env python3
"""Apply same-length string/byte patches to an MPC2500 update image and build an emulator flash image.
usage: patch_image.py in.bin out_update.bin out_flash.bin  'offset:old=new' ...
Each patch is checked against the existing bytes (old text must match) so wrong offsets are refused."""
import sys, subprocess, os
src, upd, fl = sys.argv[1:4]
d = bytearray(open(src, 'rb').read())
for p in sys.argv[4:]:
    off, rest = p.split(':', 1); old, new = rest.split('=', 1)
    off = int(off, 0); old = old.encode(); new = new.encode()
    assert len(old) == len(new), 'patch must keep length: ' + p
    assert d[off:off + len(old)] == old, 'old bytes do not match at %#x: %r' % (off, bytes(d[off:off + len(old)]))
    d[off:off + len(new)] = new
open(upd, 'wb').write(d)
subprocess.check_call([sys.executable, '-I', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mkflash.py'), upd, fl])
