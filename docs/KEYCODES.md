# MPC2500 OS 1.24 internal key codes

Measured in the emulator: while a matrix cell is held, the OS stores its key code at `0x882173CC` and sets a per-key "held" byte at `0x882174E8 + code` (helper at `0x8855EA48` = `IsKeyDown(struct 0x88217320, code)`). Codes 18 never appeared (no matrix cell produces it).

| code | dec | button | matrix cell (col,row) |
|---|---|---|---|
| 0x03 | 3 | CURSOR UP | 3,3 |
| 0x04 | 4 | CURSOR DOWN | 3,2 |
| 0x05 | 5 | CURSOR LEFT | 2,3 |
| 0x06 | 6 | CURSOR RIGHT | 4,3 |
| 0x07 | 7 | F1 | 2,9 |
| 0x08 | 8 | F2 | 3,9 |
| 0x09 | 9 | F3 | 4,9 |
| 0x0a | 10 | F4 | 4,8 |
| 0x0b | 11 | F5 | 3,8 |
| 0x0c | 12 | F6 | 2,8 |
| 0x0d | 13 | MAIN | 0,2 |
| 0x0e | 14 | WINDOW | 1,2 |
| 0x0f | 15 | NOTE REPEAT | 3,6 |
| 0x10 | 16 | ERASE | 4,6 |
| 0x11 | 17 | INPUT THRU | 0,8 |
| 0x13 | 19 | 16 LEVELS | 1,9 |
| 0x14 | 20 | FULL LEVEL | 0,9 |
| 0x15 | 21 | TRACK MUTE | 2,10 |
| 0x16 | 22 | NEXT SEQ | 1,8 |
| 0x17 | 23 | PAD BANK A | 0,10 |
| 0x18 | 24 | PAD BANK B | 1,10 |
| 0x19 | 25 | PAD BANK C | 3,10 |
| 0x1a | 26 | PAD BANK D | 4,10 |
| 0x1b | 27 | PLAY START | 4,0 |
| 0x1c | 28 | PLAY | 3,0 |
| 0x1d | 29 | STOP | 2,0 |
| 0x1e | 30 | REC | 0,0 |
| 0x1f | 31 | OVER DUB | 1,0 |
| 0x20 | 32 | STEP < | 0,1 |
| 0x21 | 33 | STEP > | 1,1 |
| 0x22 | 34 | GO TO | 2,1 |
| 0x23 | 35 | BAR << | 3,1 |
| 0x24 | 36 | BAR >> | 4,1 |
| 0x25 | 37 | 0 | 1,4 |
| 0x26 | 38 | 1 | 0,5 |
| 0x27 | 39 | 2 | 1,5 |
| 0x28 | 40 | 3 | 2,5 |
| 0x29 | 41 | 4 | 0,6 |
| 0x2a | 42 | 5 | 1,6 |
| 0x2b | 43 | 6 | 2,6 |
| 0x2c | 44 | 7 | 0,7 |
| 0x2d | 45 | 8 | 1,7 |
| 0x2e | 46 | 9 | 2,7 |
| 0x2f | 47 | +/- | 0,4 |
| 0x30 | 48 | DATA - | 3,4 |
| 0x31 | 49 | DATA + | 4,4 |
| 0x32 | 50 | Q1 AFTER (inferred) | 4,7 |
| 0x33 | 51 | Q2 AFTER (inferred) | 3,7 |
| 0x34 | 52 | Q3 AFTER (inferred) | 4,5 |
| 0x35 | 53 | Q4 AFTER (inferred) | 3,5 |
| 0x36 | 54 | SHIFT | 2,2 |
| 0x37 | 55 | MODE | 0,3 |
| 0x38 | 56 | UNDO | 4,2 |
| 0x56 | 86 | ENTER | 2,4 |
| 0x8a | 138 | TAP TEMPO | 1,3 |

## LED indices (from the OS LED refresh routine, `0x8855F23C`)
LED index = column*4 + bit of the 5-byte LED table at `0x881C005C`: 0 REC, 1 MODE, 2 FULL LEVEL, 3 PAD BANK A, 4 OVER DUB, 5 WINDOW, 6 16 LEVELS, 7 PAD BANK B, 11 TRACK MUTE (probable), 12 Q2 AFTER, 13 Q4 AFTER, 14 PLAY, 15 PAD BANK C, 16 Q1 AFTER, 17 Q3 AFTER, 18 UNDO, 19 PAD BANK D (8-10 not identified).

## NOTE REPEAT evidence
The pad-hit handler (`0x8856F5F4`) tests `IsKeyDown(16)` (ERASE) first, then `IsKeyDown(15)`; only if 15 is held (or the latch flag at struct+0x15D is set) does it enter the repeat path. In the emulator, holding cell 3,6 with a pad held while a sequence plays made that path run 72 times in 30M steps; no other cell did (0).
