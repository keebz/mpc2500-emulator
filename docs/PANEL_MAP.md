# MPC2500 front-panel matrix map (stock OS 1.24, found in the emulator)

5 columns x 11 rows. "verified" = the OS visibly responded as that button should; "inferred" = placed by layout/LED evidence; "unknown" = no name yet.

| col,row | button | confidence | evidence | emulator key |
|---|---|---|---|---|
| 0,0 | REC | verified | OS drives LED 0 (col0 bit0) from record state; LED lights while this cell is held | R |
| 1,0 | OVER DUB | verified | OS drives LED 4 (col1 bit0) from overdub state; LED lights while this cell is held | O |
| 2,0 | STOP | verified | stops a running sequence | X |
| 3,0 | PLAY | verified | plays from current position (002.01.00 stayed put) | Space |
| 4,0 | PLAY START | verified | restarts from 001.01.00 when started at 002.01.00 | Z |
| 0,1 | STEP < | verified | 001.01.48 -> 001.01.24 | , |
| 1,1 | STEP > | verified | 001.01.00 -> 001.01.24 | . |
| 2,1 | GO TO | verified | opens Locate window | G |
| 3,1 | BAR << | verified | 003.01.00 -> 002.01.00 | Page Up |
| 4,1 | BAR >> | verified | 001.01.00 -> 002.01.00 | Page Down |
| 0,2 | MAIN | verified | returns to main page from any page | M |
| 1,2 | WINDOW | verified | opens Sequence name window | W |
| 2,2 | SHIFT | verified | with RIGHT the cursor stays in field (digit select) | Left Shift |
| 3,2 | CURSOR DOWN | verified | cursor moves to next field down | Down |
| 4,2 | UNDO | verified | clears the UNDO LED after a recording | U |
| 0,3 | MODE | verified | its LED blinks after pressing | D |
| 1,3 | TAP TEMPO | verified | 4 taps changed tempo to 179.1 | T |
| 2,3 | CURSOR LEFT | verified | moves cursor back from Loop to Sq | Left |
| 3,3 | CURSOR UP | verified | cursor moves up to Now field | Up |
| 4,3 | CURSOR RIGHT | verified | cursor moves Sq -> Loop | Right |
| 0,4 | +/- | inferred | keeps numeric entry open; by elimination next to 0/ENTER | \ |
| 1,4 | 0 | verified | typed 00 | 0 |
| 2,4 | ENTER | verified | commits a typed value | Return |
| 3,4 | DATA - | verified | Tr 01 -> 00 ALL TRACKS | - |
| 4,4 | DATA + | verified | Sq 01 -> 02, Tr 01 -> 02 | = |
| 0,5 | 1 | verified | typed 01 | 1 |
| 1,5 | 2 | verified | typed 02 | 2 |
| 2,5 | 3 | verified | typed 03 | 3 |
| 3,5 | Q4 AFTER | inferred | latching LED key, code 53 |  |
| 4,5 | Q3 AFTER | inferred | latching LED key, code 52 | A |
| 0,6 | 4 | verified | typed 04 | 4 |
| 1,6 | 5 | verified | typed 05 | 5 |
| 2,6 | 6 | verified | typed 06 | 6 |
| 3,6 | NOTE REPEAT | verified | OS pad handler tests key code 15 (after ERASE=16); holding it switches the handler to its repeat path (72 vs 0 passes); no LED/latch |  |
| 4,6 | ERASE | verified | opens Erase window | E |
| 0,7 | 7 | verified | typed 07 | 7 |
| 1,7 | 8 | verified | typed 08 | 8 |
| 2,7 | 9 | verified | typed 09 | 9 |
| 3,7 | Q2 AFTER | inferred | latching LED key, code 51 |  |
| 4,7 | Q1 AFTER | inferred | latching LED key, code 50; JJ OS manual: AFTER exists per Q control (Q1..Q4); order from OS code order | N |
| 0,8 | INPUT THRU | verified | opens Input thru page | I |
| 1,8 | NEXT SEQ | verified | opens Next Sq grid page | Q |
| 2,8 | F6 | verified | GO2FX on Input thru page | F6 |
| 3,8 | F5 | verified | INPUT on Input thru page (no change there); CLOSE in popups | F5 |
| 4,8 | F4 | verified | SETUP page; TR+ on main | F4 |
| 0,9 | FULL LEVEL | verified | latching LED key (LED 2); only other LED key beside 16 LEVELS in that group | V |
| 1,9 | 16 LEVELS | verified | opens Assign 16 Levels window | L |
| 2,9 | F1 | verified | L/PAN page; T.C. on main | F1 |
| 3,9 | F2 | verified | L/OUT page; CLICK on main | F2 |
| 4,9 | F3 | verified | FXSEND page | F3 |
| 0,10 | PAD BANK A | verified | key codes 23-26 = A,B,C,D in order; bank A LED lit at start | F7 |
| 1,10 | PAD BANK B | verified | LED A->B (col1 bit3) | F8 |
| 2,10 | TRACK MUTE | verified | opens Track mute page | K |
| 3,10 | PAD BANK C | verified | LED A->(col3 bit3) | F9 |
| 4,10 | PAD BANK D | verified | LED A->(col4 bit3) | F10 |

Not in the matrix: 16 pads (pressure via ADC), Q1/Q2 sliders, Q3/Q4 knobs, REC GAIN, MAIN VOLUME, data wheel (quadrature on port F), footswitches.
Counts: verified 50, inferred 5 (+/-, Q1-Q4 AFTER), unknown 0. See docs/KEYCODES.md for the OS key codes.

Emulator extras: mouse wheel or `[` / `]` = data wheel; Tab (hold) = fast-forward; Esc = quit; click any grid cell to hold that key.

## Pads
Pad n (1-16, PAD 1 bottom-left) is ADC input raw index 4*(3-(n-1)/4)+(n-1)%4; verified by the Assign 16 Levels window (A01..A16). Hits register at ADC >= 10.
NOTE REPEAT/AFTER remain inferred: the manual's "soft-key row changes while NOTE REPEAT is held" could not be reproduced in the emulator with any cell held.
