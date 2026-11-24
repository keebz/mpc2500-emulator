# Building the downloadable applications

The releases are produced by **GitHub Actions** (`.github/workflows/release.yml`): pushing a tag such as `v1.0.0` builds the Linux and macOS applications and attaches them to a GitHub release. You can also build them yourself with the scripts here.

| Output | Built by | Contents |
|---|---|---|
| `MPC2500-Emulator-<ver>-linux-x86_64.tar.gz` | `packaging/linux/build.sh` | A folder with the program (`MPC2500 Emulator`) and its `resources/`; needs only the C library |
| `MPC2500-Emulator-<ver>-linux-x86_64.AppImage` | the same script, if `APPIMAGETOOL` points at appimagetool | One file that runs on most desktop distributions |
| `MPC2500-Emulator-<ver>-macOS-arm64.zip` / `.dmg` | `packaging/macos/build.sh` on an Apple Silicon Mac | `MPC2500 Emulator.app` |
| `MPC2500-Emulator-<ver>-macOS-x86_64.zip` / `.dmg` | `packaging/macos/build.sh` on an Intel Mac | `MPC2500 Emulator.app` |

## How the apps are put together
- **SDL2 is built from the official source release and linked statically** (`packaging/build_sdl.sh`). On Linux the video/audio back-ends (X11, Wayland, PulseAudio, ALSA, PipeWire) are loaded at run time by SDL itself, so the finished program depends only on the C library and works on desktops with different library versions.
- **No Python and no terminal are needed at run time.** The panel artwork is generated at build time (`tools/make_skin.py`) and shipped inside the app (`resources/skin` on Linux, `Contents/Resources/skin` on macOS).
- **Resources and user files.** A build is recognised as a packaged app when a `resources/` folder is next to the program (Linux tarball), `../share/mpc2500-emulator` exists (AppImage) or `../Resources` exists (macOS bundle). The app then keeps the user's files in `~/MPC2500 Emulator/` (`firmware/`, `card/`, `card-files/`, `settings.txt`), shows a firmware chooser window instead of using the command line, accepts files dropped on the window, and writes changes to the card image back when it exits.
- Environment overrides for testing: `MPC2500_RESOURCES` (resource folder) and `MPC2500_HOME` (user folder).

## Building locally
Linux (Ubuntu/Debian build dependencies for SDL2, then the script):
```bash
sudo apt install build-essential curl python3-pil fonts-liberation libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
     libxfixes-dev libxkbcommon-dev libwayland-dev wayland-protocols libdrm-dev libgbm-dev libegl1-mesa-dev libgl1-mesa-dev \
     libasound2-dev libpulse-dev libpipewire-0.3-dev libdecor-0-dev libdbus-1-dev
packaging/linux/build.sh 1.0.0          # add APPIMAGETOOL=/path/to/appimagetool for the AppImage
```
macOS (Xcode command-line tools installed; the artwork must exist, generate it on Linux or with Pillow plus the Liberation fonts):
```bash
packaging/macos/build.sh 1.0.0
```
Results are written to `dist/`.

## Notes
- **macOS and Gatekeeper.** The app is signed ad hoc (required on Apple Silicon) but **not notarised**, which needs a paid Apple Developer account. On first launch macOS says it cannot verify the developer: right-click the app, choose *Open*, then *Open* again (or allow it under *System Settings > Privacy & Security*). This is needed once.
- **AppImage and FUSE.** AppImages need FUSE 2 on the host. Recent Ubuntu releases do not install it by default (`sudo apt install libfuse2t64`); the `.tar.gz` build needs nothing and is the fallback.
- **The macOS build has not been run on a Mac by the author**; it is built and tested only by the CI workflow. Please report problems.
- Apple laptops have no Insert key, and function keys need the Fn key: use the mouse for those panel buttons, or edit `panel_map.txt` in the app's Resources.
