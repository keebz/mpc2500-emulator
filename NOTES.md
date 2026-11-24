# MPC2500 Emulator - engineering notes

A short, public summary of how the emulator is built and what has been verified. It deliberately holds no firmware content and no third-party material.

## Architecture
- `emu/sh3.c`, `emu/dsp.c`: SH7727 CPU (SH-3 integer set plus the SH3-DSP unit: X/Y memory, modulo addressing, hardware repeat loops).
- `emu/soc.c`, `emu/board.c`: on-chip peripherals (interrupt controller, TMU, DMAC, SCIF, SIOF, A/D converter, ports, bus state controller) and the board (SDRAM/SODIMM with SPD, flash, CompactFlash, LCD, front panel).
- `emu/flash.c`: one 8 MiB AM29LV641DL-style device with real command sequences (unlock, program, sector erase, autoselect, CFI).
- `emu/ata.c`: CompactFlash card (FAT16 image) with CHS/LBA addressing.
- `emu/audio.c`: decodes the SIOF stream (left group then right group, 20-bit samples in 16-bit slots) to stereo.
- `emu/ui_sdl.c`, `emu/launcher.c`, `emu/app.c`: the window, the firmware/card chooser, the start-up notice, the user folder and the virtual unit (`unit.flash`).
- `emu/fwload.c`: recognises firmware files (full builds, run-in-place images, OS-only payloads, whole-flash dumps).
- `emu/script.c`: scripted control for tests (`--exp`): key taps, pad hits, LCD dumps, audio capture, flash save, memory watchpoints.

## Tests
- `tests/cpu_diff.py`, `tests/dsp_diff.py`: the CPU and DSP against independent Python models written from the Renesas manuals (no firmware needed).
- `tests/golden.py`: regression suite (screens, LEDs, audio hashes) over real firmware boots and key sequences; goldens in `tests/golden.json`.
- `tests/mix_check.py`: multi-voice audio mixing check.
- `tests/install_chain.py`: the install chain through the firmware's own installers, compared byte for byte.
- Sanitiser build: `make -C emu asan`.

## Findings that shaped the design
- **DSP repeat loops:** loops of 1-3 instructions arm at the last instruction, longer ones one instruction earlier, and must re-arm after every pass. Getting this wrong produced a loud buzz when several voices played.
- **Audio path:** the OS feeds the SIOF in 8-word frames; the live window resamples with a wide rate band and a start-up cushion so brief scheduling stalls do not mute playback.
- **Q-Links:** each of the four analogue mux lines carries one control on A/D channel AN2; the 32-bit ADDR read lays out the converted values as [H,L,L,H].
- **LEDs:** the firmware writes the LED row data to a port before it strobes a column; the column latches the data while strobed. The mapping from firmware LED bits to keys is `led_map.txt` (tracked at the repository root and copied into the skin).
- **Flash:** the decoder must not treat an F0 data word in a program cycle as the reset command; this stalled every OS update until fixed.
- **Install paths:** the boot block and the stock OS updater only erase and write the OS area (`0x010000`-`0x0EFFFF`, 917,488 bytes usable). An OS larger than that has to come in through an OS with its own installer and overwrites the settings sector and then the start of the internal memory (see the Quick Start).
- **Memory:** the unit starts with the stock 16 MB; 128 MB is the optional expansion. The card image size is unrelated.
- **Virtual unit:** the whole flash chip is kept in `unit.flash` (saved atomically at exit, before restarts and a few seconds after the flash last changed).

## Known limitations
See the README. In short: instruction timing is an average, not cycle accurate; MIDI, USB, audio inputs and the analogue knobs are not emulated; the macOS app is built by CI and not notarised; nothing here replaces testing on real hardware.

## Debug hooks
Environment variables used by the tests: `LIVE_EVENTS`, `LIVE_FRAMES`, `LIVE_QSET`, `LIVE_WHEEL`, `LIVE_DROP`, `AUDIO_DUMP`, `AUDIO_DEBUG`, `LED_TRACE`, `FLASH_LOG`, `FLASH_PLOG`, `ATA_LOG`, `LAUNCHER_ACTIONS`, `LAUNCHER_SHOT`, `DISCLAIMER_TEST`, `FAULT_STRICT`, `SODIMM_MB`, `CARD`.
