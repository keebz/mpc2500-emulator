# MPC2500 emulator

A from-scratch emulator of the Akai MPC2500 (Hitachi/Renesas SH7727, SH3-DSP) for **testing custom and stock firmware** on a computer:
CPU + DSP, SDRAM/SODIMM, flash, CompactFlash (FAT16), T6963C LCD, front panel (keys, 16 pads, DATA wheel, LEDs), audio output.
Drop a firmware file in `firmware/`, start it, and use the front panel with mouse and keyboard.

Not affiliated with Akai Professional. **No Akai firmware or manuals are included**; supply your own firmware files.
It is a development tool: hardware that has not been needed so far (USB, MIDI, Q-Link ADC inputs, SCI/RTC/WDT interrupts) is not modelled.

## Platforms
Developed and tested on **Linux (Ubuntu) only**. Needs a C11 compiler with GCC-style extensions (gcc or clang), `make`, SDL2, and Python 3 with Pillow.
- Linux: supported.
- macOS: should build with `brew install sdl2 python pillow` (clang accepts the GCC extensions used), **untested**.
- Windows: not supported yet. The core is portable C, but the scripted-experiment mode (`script.c`) uses `fork()` and the firmware-folder scan uses `dirent.h`.
  MSYS2/MinGW would need those two parts adapted. The test suite also relies on `fork`.

## Set up (Ubuntu / Debian)
```bash
sudo apt install build-essential libsdl2-dev python3 python3-pil
git clone <this repo> mpc2500 && cd mpc2500
cp /path/to/MPC2500.BIN firmware/        # your firmware file(s)
./run_live.sh
```
`run_live.sh` generates the front-panel artwork (`tools/make_skin.py`), builds `emu/mpc2500emu`, and starts it.
Firmware choice: if `firmware/` holds exactly one usable file it is used; otherwise pass `--firmware firmware/NAME`.
`emu/mpc2500emu --list` shows what was found and how each file is recognised. `emu/mpc2500emu --help` lists all options.

Accepted firmware: full Akai/JJOS builds (`.bin`), run-in-place test images (`.osx`, `.os3`), OS-only `.SOS` (needs a full build in the same folder as boot-block donor), and 8 MiB flash dumps. Files are only read; the emulator's flash is in memory and never written back unless you pass `--save-flash`.

## CompactFlash card and RAM
- Put a FAT16 image at `card/card.img` (or pass `--card FILE`). It is copy-on-write: your file is never modified.
- Build one without root: `python3 tools/mkfat16.py card/card.img 128 FILE1 FILE2 ...` (128 MB image holding those files).
- `--ram MB` sets installed memory (default 128 = SODIMM; `0` = stock 16 MB).

## Controls
| Input | Action |
|---|---|
| Click a panel button | holds it while the mouse button is down |
| Click a pad | hit (left = medium, right = soft, middle = hard) |
| Mouse wheel, drag on the jog wheel, `[` `]` | DATA wheel |
| Keyboard | mapped per `panel_map.txt` (REC `R`, OVER DUB `O`, STOP `X`, PLAY `Space`, PLAY START `Z`, MODE `D`, MAIN `M`, DATA -/+ `-` `=`) |
| Hold `Tab` | fast-forward |
| `--audio-shift N` | output level: 20-bit OS samples >> N (default 4 = DAC-faithful; the live window adds 2x gain and a soft limiter) |
| `Ctrl+R` | power cycle |
| `Esc` | quit |

Speed: about 87 M emulated instructions/s on an i7-6700K, real time needs 66 M/s. Slower machines will run below real time (use no other heavy load).

## Speed
The emulator needs about 84 M emulated instructions per second for real time. `make -C emu pgo` builds a profile-guided binary (~10 % faster; trains on `firmware/` or `FIRMWARE=file`).
If the host is too slow the sound plays slightly flat instead of breaking up. `STEPS_PER_FRAME=n` changes the CPU budget per audio frame (default 1900).

## Build only / tests
```bash
make -C emu all          # window build (needs SDL2)      make -C emu hl    # headless build used by tests
make -C emu asan         # address/UB sanitizer build
python3 tests/golden.py --bin emu/mpc2500emu_hl -j 7            # needs the firmware files under archive/ (see tests/golden.py)
python3 tests/golden.py --bin emu/mpc2500emu_hl --raw -j 7      # same, through the native firmware loader
```
`python3 tests/dsp_diff.py` and `python3 tests/cpu_diff.py` compare the CPU/DSP against independent models written from the Renesas manual (no firmware needed).
The golden suite boots each firmware, presses keys, and compares LCD frames and audio against recorded hashes. CPU correctness is checked against an independent reference (`tests/cpu_diff.*`).

## Layout
`emu/` emulator (sh3, dsp, board, flash, ata, lcd, soc, audio, panel, fault, dbg, fwload, script, cli, ui_sdl) | `tools/` image and artwork tools |
`tests/` golden and CPU tests | `docs/` panel map | `NOTES.md` engineering notes and open items.

## Rollback / safety
The emulator never touches real hardware or your firmware files. Some firmware (e.g. the JJOS XL `.osx` test image) overwrites the settings and internal-data flash sectors; this only affects the emulator's in-memory flash and resets on the next launch.

## License
Original code in this repository: see `LICENSE` once chosen by the owner. Not licensed for redistributing Akai firmware, manuals or ROM images.
