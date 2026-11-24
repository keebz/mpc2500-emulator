#!/usr/bin/env bash
# Builds a CompactFlash image whose AUTOLOAD folder makes the stock Akai OS load a kit at power-up (manual p.106: SAVE ENTIRE MEMORY -> F2 A.LOAD).
# It drives the emulated OS itself, so the folder is exactly what the firmware writes (project/ALL/PGM files, samples as .WAV, Input Thru file).
#   usage: tools/make_autoload_card.sh IN_CARD.img OUT_CARD.img [firmware.bin]
# IN_CARD must hold the kit in its root folder, as built by tools/mkfat16.py (KIT.PGM + samples): the script loads it through the LOAD page
# (the first program file in the list), selects the program on the PROGRAM page, then saves the entire memory as the AUTOLOAD folder.
set -euo pipefail
cd "$(dirname "$0")/.."
IN=${1:?input card image}; OUT=${2:?output card image}; FW=${3:-archive/v124/mpc2500.bin}
make -s -C emu hl
PRE="tap 0 3;wait 20000000;padhit 2 100;wait 30000000;tap 2 8;wait 30000000;tap 3 8;wait 1600000000;tap 0 2;wait 30000000;tap 0 3;wait 20000000;padhit 7 100;wait 30000000;tap 4 4;wait 20000000"
OPEN="press 0 3;wait 20000000;padhit 3 100;wait 20000000;release 0 3;wait 80000000"        # MODE + PAD 3 = SAVE page
TMP=$(mktemp -d)
CARD="$IN" SODIMM_MB=64 emu/mpc2500emu_hl "$FW" --exp "$TMP" 300000000 "PRELUDE=$PRE" \
  "save=$OPEN;wheel 1 5;wait 20000000;tap 2 8;wait 40000000;tap 3 9;wait 40000000;tap 3 8;wait 3000000000;dump done;cardsave $OUT"
echo "wrote $OUT (the last screen is in $TMP/save_done.bin: it should show the SAVE page with Free space reduced)"
