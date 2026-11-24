# MPC2500 Emulator

A from-scratch software emulator of the **Akai MPC2500** (Hitachi/Renesas SH7727 "SH3-DSP" based sampler/sequencer), built to **run and test MPC2500 firmware on a computer** with the front panel, LCD, pads, wheel, Q-Links and audio output behaving as they do on the unit.

It runs the stock Akai OS and third-party operating systems unmodified, from the same firmware files you would put on a real MPC2500, so you can try a new or modified firmware build before it goes anywhere near hardware.

<p align="center">
  <img src="docs/images/panel.png" alt="MPC2500 Emulator running stock Akai OS 1.24 with a kit loaded" width="760">
</p>

> **Disclaimer — please read.** This software is provided **"as is", without warranty of any kind**, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose and non-infringement. The authors accept **no liability** for any claim, damages or other liability arising from the use of, or inability to use, this software. It is an independent, unofficial project: it is **not affiliated with, endorsed by or supported by Akai Professional, inMusic, Renesas or any firmware author**. "Akai", "Akai Professional", "MPC" and "MPC2500" are trademarks of their respective owners and are used here only to identify the hardware being emulated; the application carries no manufacturer logos. **No Akai or third-party firmware, manuals or other copyrighted material is included**; you must supply your own firmware files and are responsible for having the right to use them.
>
> **EXPERIMENTAL SOFTWARE — ONLY FOR PEOPLE WHO KNOW EXACTLY WHAT THEY ARE DOING.** The emulator shows the full notice below **every time it starts**; you must read it to the end and click **I accept**. If you decline (or close the window) the application exits immediately.
>
> 1. **Experimental.** This is an unfinished research tool for people who understand embedded firmware, flash memory and the risks of modifying hardware. If you do not, do not use it and do not install any firmware on a real unit.
> 2. **Testing here proves nothing about real hardware.** If you create, modify, patch, port or obtain a firmware and run it in this emulator, that gives **no promise, guarantee or expectation of any kind** that it will work on a real MPC2500 — or that it is safe. The emulator does not model every device, register, timing, power or fault condition, and what it does model may be wrong in ways that are known and in ways that are not.
> 3. **Installing firmware can permanently damage your hardware.** It can leave a unit unable to start or recover ("bricked"), erase internal memory and data, void the warranty, or damage it **beyond repair**. Nothing in this project — documentation, examples, messages, test results, screenshots or emulator output — is a recommendation or approval to install anything on real hardware. You do so entirely at your own risk.
> 4. **No guarantee that the emulator's output is valid.** No representation is made about the validity, accuracy, completeness, timing, fidelity or reliability of anything it produces or reports (audio, display, timing, files and disk images, logs, fault reports, test results). Do not rely on it for decisions about hardware, firmware or data. No error here does not mean there is no error.
> 5. **No warranty.** Provided "as is" and "as available", without warranty of any kind, express, implied or statutory, including merchantability, fitness for a particular purpose, accuracy, non-infringement and quiet enjoyment.
> 6. **No liability.** To the fullest extent permitted by law the authors and contributors are not liable for any claim, loss or damage — direct, indirect, incidental, special, consequential or punitive, including damage to or destruction of hardware and loss of data — arising from the use of, or inability to use, this software or its output, whether in contract, tort (including negligence) or otherwise, even if advised of the possibility.
> 7. **You assume all risk** for the firmware you run, create, modify or install, for backups, for having the right to use any material you load, and for complying with applicable law and licences. You agree to indemnify and hold harmless the authors and contributors from claims arising from your use.
> 8. **Not affiliated** with Akai Professional, inMusic, Renesas or any firmware author; trademarks are used only to identify the emulated hardware. No firmware is supplied.

---

## Contents
- [What it is](#what-it-is)
- [Features](#features)
- [Screenshots](#screenshots)
- [What is emulated, and what is not](#what-is-emulated-and-what-is-not)
- [Firmware compatibility](#firmware-compatibility)
- [The virtual unit](#the-virtual-unit-installing-firmware-like-the-real-hardware)
- [Download and run](#download-and-run-no-terminal-nothing-to-install)
- [Quick start (from source)](#quick-start-from-source)
- [Using the front panel](#using-the-front-panel)
- [CompactFlash cards, AUTOLOAD and internal memory](#compactflash-cards-autoload-and-internal-memory)
- [Testing your own firmware](#testing-your-own-firmware)
- [Accuracy and verification](#accuracy-and-verification)
- [Performance](#performance)
- [Repository layout](#repository-layout)
- [Known limitations](#known-limitations)
- [Documentation](#documentation)
- [Licence and legal](#licence-and-legal) (MIT)

## What it is
The MPC2500 firmware expects a specific machine: an SH7727 CPU with its DSP unit, 16 MB (or more, with a SODIMM) of SDRAM, an 8 MB flash chip holding the boot block, the operating system and the internal memory, a CompactFlash slot, a 240x64 T6963C LCD, a key matrix, sixteen velocity pads read through an analogue multiplexer and the SH7727's ADC, a quadrature DATA wheel, four Q-Link controls, and an audio path fed through the on-chip SIOF.

This project models that machine, written from public documentation (the SH-3/SH3-DSP software manual, the SH7727 hardware manual, the MPC2500 operator's and service manuals) and from observing what the firmware itself does. It contains **no code from other emulators and no Akai code**.

## Features
- **Complete SH7727 CPU core**: the full SH-3 integer instruction set plus the SH3-DSP unit (parallel ALU/multiply/transfer instructions, X/Y memory, modulo addressing, hardware repeat loops). The integer core was checked against an independent model over all 65,536 opcodes; the DSP unit was checked against a second independent model over hundreds of thousands of random instructions (see [Accuracy and verification](#accuracy-and-verification)).
- **Runs unmodified firmware**: stock Akai OS and third-party OS builds, from their original files (`.bin`, `.osx`, `.os3`, OS-only `.SOS` with a boot-block donor, or an 8 MB flash dump).
- **A virtual unit that installs firmware like the real one**: the flash chip is kept in a file, the firmware's own Load-page installers decide what is accepted, and an XL OS (one larger than the stock OS area) has to be installed by an OS with its own installer, exactly as on the hardware. A developer direct-load mode is available for quick tests.
- **Real front panel**: every button, 16 velocity-sensitive pads (mouse buttons choose the velocity), a DATA wheel (mouse wheel, drag or `[` `]`), four Q-Link controls that the firmware reads through the real ADC path, LEDs driven the way the firmware drives them, and a keyboard map for the whole panel.
- **Audio**: the firmware's own 32-voice sample engine runs on the emulated DSP; its SIOF output is decoded to stereo and played through your sound card. Panning, filters, tuning, polyphony and stereo samples work.
- **Storage**: a CompactFlash card image in FAT16 (including `AUTOLOAD`), the stock 16 MB of memory by default (a 128 MB expansion can be selected), and an 8 MB AM29LV641DL-style flash with real command sequences, so firmware that writes flash behaves realistically.
- **Firmware-developer tooling**: fault reports (illegal instruction, wild jumps, unmapped accesses, boot-block writes), a headless mode, a scripting interface for repeatable tests, memory watchpoints, breakpoints, whole-flash save/restore and a regression suite.
- **Packaged apps** for Linux and macOS with everything built in: a chooser window, drag-and-drop for firmware and card images, and card changes kept between sessions.
- Built from source on Linux with SDL2 as the only dependency.

## Screenshots

The keyboard map, drawn on the panel (every yellow tag is the key for that button, blue tags are the numeric-keypad keys for the pads):

![Keyboard map](docs/images/keymap.png)

Real output of the emulated firmware (the LCD is the emulated T6963C frame buffer):

| | |
|---|---|
| ![AUTOLOAD at start-up](docs/images/lcd_autoload_loading.png) <br> The stock OS loading the kit from the card's `AUTOLOAD` folder at power-up | ![Main page after autoload](docs/images/lcd_autoload_done.png) <br> The main page afterwards, with `Pgm:KIT` loaded |
| ![Load page](docs/images/lcd_load.png) <br> `MODE` + `PAD 2` — the Load page listing the card | ![Mixer](docs/images/lcd_mixer.png) <br> The Mixer (L/PAN) page; Q1/Q2 move the level and pan of the selected pad |
| ![Slider page](docs/images/lcd_slider.png) <br> The Q-Link Slider page (`MODE` + `PAD 1`), here REAL TIME / CUTOFF1 | ![Program filter](docs/images/lcd_program_filter.png) <br> Program FILTER page; the Q-Link cutoff/resonance offsets act on an enabled filter |

## What is emulated, and what is not

| Area | Status |
|---|---|
| SH7727 CPU (SH-3 core, SH3-DSP unit, X/Y memory, repeat loops) | Emulated; verified against independent models |
| Interrupts: TMU timers, DMAC channel 0, SIOF, ADC, IRQ levels/priorities, INTEVT codes | Emulated |
| SDRAM 16 MB and SODIMM (SPD detection over the emulated I2C bus) | Emulated |
| 8 MB flash (AMD command set: program, sector/chip erase, unlock bypass, autoselect, CFI) | Emulated; boot-block writes are reported as faults |
| CompactFlash card (True-IDE: IDENTIFY, LBA and CHS reads/writes, FAT16 images) | Emulated |
| 240x64 T6963C LCD | Emulated (full command set; stand-in 8x8 character generator) |
| Key matrix (5 columns x 11 rows), LEDs | Emulated; LED data decoded from the firmware's port writes |
| Pads (16, velocity from mouse button) | Emulated through the ADC multiplexer |
| DATA wheel | Emulated as a quadrature encoder |
| Q-Link Q1-Q4 | Emulated through the ADC (Q1/Q2 verified in the Mixer page; Q3/Q4 assigned by scan order) |
| Audio output (stereo, 44.1 kHz) | Emulated |
| MIDI in/out | **Not emulated** (the UART reports idle) |
| USB device/host | **Not emulated** (the OS enables the interrupts; they never fire) |
| Audio inputs, digital in/out, REC GAIN / MAIN VOLUME knobs | **Not emulated** (the knobs are drawn but not connected) |
| Hard-disk and CD-drive options | **Not emulated** |
| Real-time clock, watchdog, power sequencing | **Not emulated** |

## Firmware compatibility

The files below boot to their user interface in the emulator. Only the firmware you supply is used; none is distributed here.

| Firmware | File type | Result |
|---|---|---|
| Akai OS 1.24 (stock) | full build (`.bin`) | Boots, runs, loads/saves to the card, plays samples, Q-Links work |
| Third-party OS build that fits the stock OS area | full build (`.bin`) | Boots to its own main page (time-code display) |
| XL OS (larger than the stock OS area) | run-in-place test image (`.osx`) | Installed in the virtual unit through an OS with its own installer (see below), then boots to its main page; Q-Links work with its filters |
| Other XL OS images | `.osx`, `.os3` | Install the same way; they boot to their welcome dialog or main page |

<p>
  <img src="docs/images/lcd_custom_os_fits.png" width="48%" alt="A third-party OS that fits the stock OS area">
  <img src="docs/images/lcd_xl_os.png" width="48%" alt="An XL OS (larger than the stock OS area)">
</p>

"Boots" means the regression suite reaches and checks the screen shown; it does not mean every feature of every firmware has been exercised. Please report differences you find against real hardware.

## Download and run (no terminal, nothing to install)

Ready-to-run applications for Linux and macOS are attached to each release on the project's **Releases** page. Everything they need is inside; there is nothing else to install.

| System | File | How to start |
|---|---|---|
| **Linux** (x86_64) | `MPC2500-Emulator-<version>-linux-x86_64.tar.gz` | Extract it, then double-click **MPC2500 Emulator** (if your file manager asks, choose *Run*) |
| Linux (alternative) | `MPC2500-Emulator-<version>-linux-x86_64.AppImage` | Right-click > Properties > allow executing, then double-click (needs FUSE 2: `libfuse2` on Ubuntu 22.04+) |
| **macOS, Apple Silicon** (M1 and later) | `MPC2500-Emulator-<version>-macOS-arm64.zip` (or `.dmg`) | Open it, drag **MPC2500 Emulator** to *Applications*, then right-click > *Open* the first time |
| **macOS, Intel** | `MPC2500-Emulator-<version>-macOS-x86_64.zip` (or `.dmg`) | Same as above |

On the first start the app creates a folder called **MPC2500 Emulator** in your home folder and shows a chooser. **Drag your firmware file onto the window** (start with the stock Akai OS), pick it in the list and click **Flash OS**: that creates your virtual unit and starts it. The chooser also lets you add a CompactFlash card image (drop a `.img` file), create a blank card, or build a card from a folder of files, all with the mouse:

<p align="center"><img src="docs/images/launcher.png" alt="The firmware chooser" width="640"></p>

Next time the chooser appears again: press Enter (**Power on**) to start the unit as you left it. To get kits and sounds into the emulator, use the card panel in the chooser (**1 Card-files folder**, copy your files in, **2 Build card**, then Power on). While it runs: drop another firmware or card file on the window to switch, `Ctrl+O` returns to the chooser, and what the firmware saves to the card is kept. macOS shows a one-time security prompt because the app is not notarised by Apple (see [packaging/README.md](packaging/README.md#notes)). To build the applications yourself see [packaging/README.md](packaging/README.md).


## The virtual unit: installing firmware like the real hardware

The packaged app behaves like a real MPC2500, including the way firmware gets onto it. The emulator keeps the contents of the unit's flash chip (boot block, installed OS, settings and the **internal memory**) in a file, `unit.flash`, in your **MPC2500 Emulator** folder. Whatever is installed stays installed between runs.

<p align="center"><img src="docs/images/launcher.png" alt="The chooser with the virtual unit" width="640"></p>

| Chooser button | What it does |
|---|---|
| **Power on** (Enter) | Starts the unit with whatever OS is installed in it |
| **Flash OS** (I) | Writes the selected OS build to the unit like the Akai updater: only the OS area is rewritten; settings and internal memory stay. Use it to create the unit the first time (pick the stock Akai OS) |
| **Developer direct load** (D) | Developer shortcut: loads any image straight into a temporary flash and **skips the installer**. This is *not* how the hardware works; nothing is kept |
| **Reset unit** (X) | Erases the unit (OS, settings, internal memory) |

The real rules are enforced by the firmware itself, which runs unmodified. The chooser's **Flash OS** only accepts an OS that fits the stock OS area, exactly as the boot block and the Akai updater do. A larger OS has to go in through an OS with its own installer; see the next section for what that does to the internal memory.

### Installing an Extra Large (XL) OS

In this document an OS image that is **larger than the stock OS area** is called an **Extra Large (XL)** OS, or XL firmware for short.

> [!CAUTION]
> **DANGER: installing an XL OS overwrites part of the internal memory, and it can leave a real unit unusable.**
>
> An XL OS is written over the settings sector and then over the start of the internal memory (see the flash map below). Everything stored in the overwritten area is destroyed. On real hardware this corruption of the internal memory **can happen, and has happened with certain third-party XL firmware in the past**. A corrupted internal memory can make the unit **get stuck in a permanent boot loop**. Installing XL firmware on a real unit is entirely at your own risk, and a unit that no longer starts may not be recoverable.
>
> **Trying to recover (there is NO guarantee that this will work, and this project has not verified it on hardware):**
>
> 1. Prepare a CompactFlash card with the original Akai OS update file (`MPC2500.SOS` for the boot-block updater) and, if you have it, the card's `INTERNAL` folder with the factory data and an `AUTOLOAD` folder with a memory save you trust (the manual: a folder named AUTOLOAD in the card's root is loaded automatically when the unit starts with the card inserted).
>
> 2. Power the unit on with the card inserted so that the boot block's OS UPDATE screen installs the original Akai OS. The boot block does this by itself when it finds no valid OS. If an OS is present but keeps restarting, the boot code of the emulated unit opens the OS UPDATE screen when one of the keys OVER DUB, STEP >, WINDOW, TAP TEMPO, 0, 2 or 5 is held while powering on; that combination has not been verified on a real unit.
>
> 3. Once the original OS runs, let the AUTOLOAD folder load, then save to INTERNAL MEMORY (SAVE ALL PROGRAMS & SAMPLES and SAVE ALL SEQS & SONGS). The manual says this erases the internal flash and replaces its data, which is how the factory data is restored. This may rebuild a valid internal memory.
>
> 4. If the original OS still restarts because of the damaged internal memory, a firmware that clears the internal-memory headers is reported to be needed first (run it once, then install the original Akai OS and repeat step 3). No such firmware is included here. If the unit cannot start the OS UPDATE screen at all, it needs a technician.

The unit's 8 MiB flash chip is shared by the boot block, the OS, the settings and the **internal memory**. This is the map the emulator uses (it comes from tracing the real Akai OS and boot block):

| Flash address | Size | What lives there |
|---|---|---|
| `0x000000` - `0x00FFFF` | 64 KiB | Boot block. Never rewritten by any updater |
| `0x010000` - `0x0EFFFF` | 14 sectors of 64 KiB (917,504 bytes) | **The OS area.** The last 16 bytes hold the OS checksum (at `0x0EFFF0`), so an OS can be at most **917,488 bytes** |
| `0x0F0000` - `0x0FFFFF` | 1 sector | The OS's settings |
| `0x100000` and up | to the end of the chip (`0x7FFFFF`) | **Internal memory** (sequences, songs, programs, samples). The stock OS keeps the header of its sequences-and-songs area at `0x100000` (sector 16) and the header of its programs-and-samples area at `0x2A0000` (sector 42) |

**The boot block and the stock Akai OS updater only erase and write the OS area** (`0x010000` - `0x0EFFFF`). They refuse any OS image that does not fit in it (the stock OS answers "File too big: Can't execute!!"). This is how the real unit behaves, and the emulator shows it because the real firmware code is running.

An XL OS **larger than the OS area** can still be installed, but only by an OS that contains its **own installer** and writes beyond the stock limit. It cannot be done by the stock updater. When such an installer runs, the image is laid out contiguously in flash starting at `0x010000`, and **whatever does not fit in the OS area overwrites the flash that follows it**:

1. The first 917,488 bytes of the image fill the OS area.
2. Next it **erases and overwrites the settings sector** (`0x0F0000`). The new OS then writes its own settings there.
3. The rest of the image **erases and overwrites the internal memory, starting at its very first sector (`0x100000`) and continuing upwards, one 64 KiB sector at a time**, for as many sectors as the image needs. That includes the stock OS's sequences-and-songs header.

So an XL OS takes its extra space **out of the internal memory**. Everything stored in the overwritten sectors is destroyed, the start of the internal memory is no longer a valid stock layout, and the unit's own warning says so ("All memory contents will be lost!!"). Sectors beyond the end of the image are not erased by the installer, but with the header at `0x100000` gone they are no longer reachable by the stock OS.

Measured on the emulated unit with real XL firmware (the procedure applies to any XL OS):

| Image | Size | Flash it occupies | Beyond the OS area | Internal-memory sectors overwritten |
|---|---|---|---|---|
| Akai OS 1.24 (stock) | 784,916 B | `0x010000` - `0x0BFA13` | none | none |
| A third-party OS that fits the OS area | 894,040 B | `0x010000` - `0x0DA457` | none (fits) | none |
| XL OS, smallest | 1,175,052 B | `0x010000` - `0x11EE0B` | 192,028 B | sectors 16-17 (`0x100000` - `0x11FFFF`, 128 KiB) |
| XL OS, medium | 1,331,048 B | `0x010000` - `0x144F67` | 348,024 B | sectors 16-20 (`0x100000` - `0x14FFFF`, 320 KiB) |
| XL OS, largest | 1,503,360 B | `0x010000` - `0x16F07F` | 520,336 B | sectors 16-22 (`0x100000` - `0x16FFFF`, 448 KiB) |

For the largest XL OS above the installer was traced in detail: it erased 23 consecutive sectors, `0x010000` - `0x17FFFF` (the OS area, the settings sector and 8 internal-memory sectors), and programmed `0x010000` - `0x16F07F` plus its settings. The table lists the sectors that receive image data; an installer may erase more than it writes.

**Going back to the stock OS.** Once the start of the internal memory has been overwritten, the stock OS no longer finds valid internal-memory headers. The recovery reported for the real unit is: run a "clear internal" OS once (it erases the two header sectors), install the normal Akai OS, then restore your internal data by loading your INTERNAL folder from the card and saving it to INTERNAL MEMORY. No such tool is included here.

**The procedure, for any XL firmware.** (1) Put an OS with its own installer on the unit (it must fit the OS area, so that the stock updater accepts it). (2) Put the XL image on the card. (3) Install it from that OS's Load page. (4) Power cycle. In the emulator's virtual unit this runs the real installer code: the installer in the test firmware lists an XL file only when it is named with a `.OSX` extension on the card (a file named `.OS3` is not listed), and you press `F6` (DO IT) on the Load page, then `F5` (DO IT) in the update dialog. This sequence is covered by the automated test `tests/install_chain.py` and compared byte for byte with the firmware files.

*How the real unit behaves after such an install, beyond what the firmware itself shows, has not been verified on hardware.*

**Safety.** Nothing here changes what you must do on real hardware: a firmware that installs and runs in the emulator is still not proven to be safe on a real unit (see the disclaimer at the top).

## Memory and card size

- **Memory (RAM):** the emulated unit starts with the **stock 16 MB** of memory, as shipped. To emulate the optional 128 MB expansion (EXM128), click **RAM: 16 MB** in the chooser (or press `M`) to switch it to 128 MB, or pass `--ram 128` on the command line. A kit that is larger than the free sample memory will not load with the stock 16 MB, just as on a real unit.
- **Card size:** the card image is a separate file (a virtual CompactFlash card) and has nothing to do with the unit's memory. The 128 MB that you see is only the default size the emulator gives a new card; **Build card** makes it larger when your files need more. The unit's memory is never stored on the card.

## Where the app keeps your files (and how to uninstall)

The first time the packaged app starts it creates a folder called **MPC2500 Emulator** in your **home folder**: `/Users/<you>/MPC2500 Emulator` on macOS, `/home/<you>/MPC2500 Emulator` on Linux. The chooser also shows this location at the bottom of its window.

| Inside the folder | What it is |
|---|---|
| `firmware/` | the firmware files you added |
| `card-files/` | the kits and sounds you want on the card (you fill this) |
| `card/` | the card image the emulator uses (`card.img`, and `card.img.bak` after a rebuild) |
| `unit.flash` | your **virtual unit**: the flash chip with the installed OS, settings and internal memory (8 MB) |
| `settings.txt` | the app's remembered choices (last firmware, card) |

**Deleting the app does not delete this folder**, and the folder is never removed automatically. To uninstall completely, delete the app *and* the `MPC2500 Emulator` folder. Copy out anything you want to keep first, such as the card image, `unit.flash` (it holds your internal memory) or your own firmware files. To make the app forget your choices without deleting your files, delete only `settings.txt`. Setting the environment variable `MPC2500_HOME` makes the app use a different folder.

## Quick start (from source)

Full step-by-step instructions, with different firmware types, are in **[QUICKSTART.md](QUICKSTART.md)**. In short, on Ubuntu/Debian:

```bash
sudo apt install build-essential git libsdl2-dev python3 python3-pil
git clone <repository-url> mpc2500-emulator && cd mpc2500-emulator
cp /path/to/your/firmware.bin firmware/
./run_live.sh
```

`run_live.sh` generates the panel artwork, builds the emulator, and opens the window. The stock OS boots to the main page in a few seconds.

## Using the front panel

| Input | Action |
|---|---|
| Click a panel button | Holds that button while the mouse button is down |
| Click a pad | Hit; left button = medium, right = soft, middle = hard |
| Mouse wheel, drag on the jog wheel, `[` and `]` | DATA wheel |
| Drag a Q-Link fader or knob | Q1-Q4 |
| Keyboard | See the picture above (`panel_map.txt` holds the map and can be edited) |
| `Tab` (hold) | Fast-forward (8x) |
| `Ctrl+R` | Power cycle (flash and card contents survive, RAM is cleared) |
| `Ctrl+O` | Return to the firmware chooser (packaged app) |
| `Ctrl+/` (macOS: `Cmd+/`) | Cheat sheet: an overlay listing every key binding (also the **Cheat Sheet** button on the top bezel) |
| **Firmware / Card** button | Top bezel, same as `Ctrl+O`: change firmware, build or eject the card |
| `Esc` | Quit |

Combinations work as on the hardware by holding one key on the keyboard and using the mouse for the other, for example `MODE` (`D`) held while clicking `PAD 1`.

## CompactFlash cards, AUTOLOAD and internal memory
- A FAT16 image at `card/card.img` is inserted automatically (or pass `--card FILE`). The emulator works on an in-memory copy; add `--save-card FILE` to keep anything the firmware saved to it.
- `python3 tools/mkfat16.py card/card.img 128 FILE...` builds a 128 MB card image containing the given files without needing root.
- A card with an `AUTOLOAD` folder in its root makes the OS load that folder at power-up, exactly as on the unit (Operator's manual, Chapter 16). `tools/make_autoload_card.sh` creates one by driving the emulated OS through *Save Entire Memory → A.LOAD*.
- "INTERNAL MEMORY" in the Load/Save pages is the unit's internal flash, not a folder on the card. It lives inside the emulated flash chip and starts blank; `--save-flash FILE` keeps it and `--flash-state FILE` starts from it again.

## Testing your own firmware
The emulator has three levels of use, described in [QUICKSTART.md](QUICKSTART.md#6-testing-your-own-firmware):

1. **Interactive** — open the window, use the panel, listen.
2. **Headless smoke test** — `emu/mpc2500emu_hl firmware.bin --steps 400000000` boots without a window and prints the CPU state, interrupt setup and a categorised **fault summary**. Illegal instructions, jumps to invalid addresses, stack corruption and writes to the flash boot block stop the run with a fault report (last 64 program counters, registers, stack).
3. **Scripted, repeatable tests** — press keys, hit pads, turn the wheel, capture the LCD and the audio, and compare against recorded results, all from a one-line script. The project's own regression suite (`tests/golden.py`) is built this way and is a good template.

## Accuracy and verification
- **CPU**: `tests/cpu_diff.py` runs all 65,536 16-bit opcodes from random machine states and compares the emulator with an independent model written from the Renesas software manual: 0 mismatches. The model's opcode legality is checked against the manual's opcode tables.
- **DSP unit**: `tests/dsp_diff.py` compares the parallel-processing, double and single data transfer instructions with a second independent model: 0 mismatches over hundreds of thousands of random instructions. The model records the places where the manual is silent or contradicts itself.
- **Audio engine**: `tests/mix_check.py` checks that voices played together equal the sum of the same voices played alone (error below 0.1 %).
- **Regression suite**: `tests/golden.py` boots each firmware and compares LCD frames and audio against recorded results across 13 scenarios (boot screens, every mode page, every key, wheel, pads, polyphony, panning, Q-Links, filters, AUTOLOAD).
- **Timing** is derived from the real clock tree (13.3 MHz crystal, 159.6 / 53.2 / 26.6 MHz CPU / bus / peripheral clocks) and the audio frame rate (44.1 kHz). The one calibrated constant is the average cost of an instruction (about 2.4 CPU cycles).

What has **not** been verified against real hardware is listed under [Known limitations](#known-limitations).

## Performance
The emulator needs to execute about **66 million emulated instructions per second** to run in real time. A desktop CPU from the last decade does this comfortably (an i7-6700K reaches roughly 100 million per second; `make -C emu pgo` builds a profile-guided binary that is about 10 % faster). If the host is too slow, the sound plays slightly flat rather than breaking up.

## Repository layout
```
emu/        the emulator (C): sh3.c (CPU), dsp.c (DSP unit), board.c (memory map), flash.c, ata.c, lcd.c, soc.c (timers, ADC, SIOF, interrupts),
            audio.c, panel.c (keys, pads, wheel, LEDs), fault.c, dbg.c, fwload.c (firmware loader), script.c, cli.c, ui_sdl.c (window)
tools/      make_skin.py (panel artwork), mkfat16.py (card images), make_autoload_card.sh, make_doc_images.py, mkflash.py, lcd2png.py, ...
tests/      golden.py + golden.json (regression suite), cpu_diff.py/cpu_ref.py, dsp_diff.py/dsp_ref.py, mix_check.py
docs/       images used by this README, panel map notes
firmware/   put your firmware files here (nothing is shipped)
card/       CompactFlash images (generated or yours; not committed)
NOTES.md    engineering notes: what was learned about the hardware and how
```

## Known limitations
- Linux and macOS only (Windows is not supported; the scripting mode uses `fork()`). The macOS app is built by the project's CI and has **not been run on a Mac by the author**; it is not notarised by Apple.
- MIDI, USB, audio inputs, digital I/O, the analogue REC GAIN / MAIN VOLUME knobs, the hard-disk and CD options, and the real-time clock are not emulated.
- The Q-Link inputs are decoded from the firmware's behaviour: Q1 and Q2 are verified on the Mixer page; Q3 and Q4 are assigned by the order the firmware scans its analogue inputs.
- The LCD uses a stand-in character generator, so text glyphs differ slightly from the real display ROM.
- Output level of the audio is not calibrated against the real DAC (`--audio-shift`).
- The flash chip's ID codes could not be verified against a datasheet (no shipped firmware reads them).
- Instruction timing is an average, not cycle accurate: busy-wait loops, or firmware that depends on exact bus timing, can behave differently from the unit.
- Installing through the firmware's own Load-page updaters is verified (stock OS, then an OS with its own installer, then XL images). Not covered: the boot-block recovery updater (`MPC2500.SOS`, used when no valid OS is present) from the chooser, power loss in the middle of an install, and any firmware or installer other than the ones above.

## Documentation
- **[QUICKSTART.md](QUICKSTART.md)** — installation and a walk-through for each firmware type.
- **[NOTES.md](NOTES.md)** — detailed engineering notes (hardware map, how each peripheral was worked out, test results).
- `docs/PANEL_MAP.md` — the key-matrix map and how confident each entry is.
- `firmware/README.txt` — where to put firmware files.

## Licence and legal
The source code of this project is released under the **[MIT License](LICENSE)**: you may use, copy, modify, merge, publish, distribute, sublicense and sell it, provided the copyright and licence notice are kept. The software is provided "as is", without warranty of any kind (see the disclaimer at the top of this file and the licence text).

Third-party components used by the packaged applications and the generated artwork (SDL2, the Liberation fonts, Pillow) and their licences are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). The firmware you run in the emulator, and its manuals, are the property of their owners and are not part of this project. The application artwork is original and carries no manufacturer logos; the screenshots in `docs/images` show output produced by firmware that the author supplied locally.
