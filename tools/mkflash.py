#!/usr/bin/env python3
"""Build an EMULATOR flash image from an MPC2500 OS update file.
Pads to 1 MiB with erased bytes (0xFF) and stores the boot loader's expected checksum at 0xEFFF0
(= ~sum of little-endian 16-bit words over [0x10000,0xEFFF0), stored as two 16-bit words).
NOTE: derived from the boot code's own validation routine (boot @0xD06-0xD86); the real updater's
layout has not been verified on hardware."""
import struct, sys, os
MODE = os.environ.get('FLASH_MODE', 'safe')   # safe: image must fit the OS window (<= 0xEFFF0 bytes); full: up to 8 MiB like JJOS XL, with a footprint warning
src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, 'rb').read())
if len(d) > 0xEFFF0:
    if MODE != 'full':
        sys.exit('REFUSED (FLASH_MODE=safe): %d bytes; the safe OS window is 0xEFFF0 = 983,024 bytes (sectors 0-14). Use FLASH_MODE=full to allow it.' % len(d))
    assert len(d) <= 0x800000, 'larger than the 8 MiB flash'
    last = (len(d) - 1) >> 16
    print('FLASH_MODE=full: image spans sectors 0-%d; overwrites settings sector 15%s%s' % (last, ' and internal sectors 16-%d (incl. SEQS&SONGS header)' % min(last, 41) if last >= 16 else '', ' and programs/samples sectors 42-%d' % last if last >= 42 else ''))
    d += b'\xff' * (((len(d) + 0xFFFF) & ~0xFFFF) - len(d))
    open(dst, 'wb').write(d); print('wrote', dst, '(no boot checksum: TEST-header image, boot runs it in place)'); sys.exit(0)
d += b'\xff' * (0x100000 - len(d))
s = sum(struct.unpack_from('<%dH' % ((0xEFFF0 - 0x10000) // 2), d, 0x10000))
c = (~s) & 0xFFFFFFFF
struct.pack_into('<I', d, 0xEFFF0, c)
open(dst, 'wb').write(d)
print('wrote', dst, 'checksum %08x' % c)
