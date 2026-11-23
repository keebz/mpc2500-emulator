Put the firmware you want to test in this folder, then run ./run_live.sh

Accepted files (the type is detected from the contents):
  *.bin / *.BIN   full Akai or JJOS build (boot block + OS), e.g. MPC2500.BIN
  *.osx *.os3     run-in-place test images
  *.sos           OS-only update; needs a full build in the same folder as "donor" for the boot block
  8 MiB dump      raw flash dump

If several files are present, pick one:  ./run_live.sh --firmware firmware/NAME.bin
List what was found:                       emu/mpc2500emu --list
No Akai firmware is distributed with this emulator; supply your own.
