# Third-party notices

The MPC2500 Emulator source code is MIT licensed (see `LICENSE`). The packaged applications and generated artwork include or are derived from the following third-party components.

| Component | Used for | Licence |
|---|---|---|
| **SDL2** (https://libsdl.org), statically linked into the packaged Linux and macOS applications | Window, input and audio output | zlib licence (https://www.libsdl.org/license.php) |
| **Liberation fonts** (Liberation Sans / Liberation Mono) | Rendered into the panel artwork (`tools/make_skin.py`), the launcher font (`emu/launcherfont.inc`) and the documentation images/PDF | SIL Open Font License 1.1 (https://github.com/liberationfonts/liberation-fonts) |
| **Pillow** (build-time only) | Artwork and documentation image generation | HPND licence |

## Not included
No Akai or third-party firmware, operating system images, manuals or other copyrighted material are included in this repository or in the packaged applications. Firmware must be supplied by the user, who is responsible for having the right to use it.

"Akai", "Akai Professional", "MPC" and "MPC2500" are trademarks of their respective owners. They are used in this project only to identify the hardware that is emulated; the project is independent and unofficial, and the application artwork carries no manufacturer logos.
