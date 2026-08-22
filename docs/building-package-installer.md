# Building the Firmware Package Installer

"The installer" is `dist/FullWashInstaller.zip` — a self-contained package that lets someone with
**no PlatformIO or developer tooling installed** flash firmware onto a board by double-clicking a
script. It's what you hand to field technicians instead of asking them to set up a dev environment.

It is **macOS-only** today (see [Limitations](#limitations) below).

## What's inside the package

```
FullWashInstaller.zip
├── README.txt          # end-user instructions (plug in board, double-click install.command)
├── install.command      # the flashing script the user double-clicks
└── firmware/
    ├── bootloader.bin
    ├── partitions.bin
    ├── boot_app0.bin
    └── firmware.bin
```

`install.command`, when double-clicked, creates an isolated Python virtualenv at
`~/.fullwash-installer/venv`, installs `esptool` into it, auto-detects the board's USB serial port
(matching `/dev/cu.usbserial-*`, `/dev/cu.wchusbserial*`, `/dev/cu.SLAB_USBtoUART`), and flashes the
four `.bin` files at their fixed addresses:

| Address | File |
|---|---|
| `0x1000`  | `bootloader.bin` |
| `0x8000`  | `partitions.bin` |
| `0xe000`  | `boot_app0.bin` |
| `0x10000` | `firmware.bin` |

(flash mode `dio`, flash freq `40m`, flash size `4MB`, baud `921600` — matching the `T-SIM7600X`
environment in `platformio.ini`). It logs everything to `~/Library/Logs/FullWashInstaller.log` for
support purposes.

## Building the package

From the repo root:

```bash
tools/package_installer.sh
```

This script (`tools/package_installer.sh`):

1. **Builds firmware if needed.** It compares the newest `.cpp`/`.h` file mtime under `src/` against
   `.pio/build/T-SIM7600X/firmware.bin`'s mtime. If the build is missing or stale, it runs
   `pio run -e T-SIM7600X` for you (see [docs/platformio-commands.md](platformio-commands.md)).
   Otherwise it reuses the existing build — so if you want to force a fresh build, run
   `pio run -t clean` first.
2. **Locates `boot_app0.bin`.** This file isn't part of the PlatformIO build output — it ships
   inside the installed `framework-arduinoespressif32` package. The script searches a couple of
   known paths under `~/.platformio/packages/framework-arduinoespressif32*/tools/partitions/`. If
   it can't find it, it errors out and tells you to run `pio run -e T-SIM7600X` once so PlatformIO
   installs the framework package.
3. **Copies the four `.bin` files** from `.pio/build/T-SIM7600X/` (plus `boot_app0.bin`) into
   `installer/firmware/` (this directory is gitignored — it's a build artifact staging area, not
   something you commit).
4. **Zips** `installer/README.txt`, `installer/install.command`, and `installer/firmware/*.bin`
   into `dist/FullWashInstaller.zip` (also gitignored), overwriting any previous zip.

Output:
```
Package created: dist/FullWashInstaller.zip
Contents:
  ...
```

## Testing before you hand it out

Don't just trust the zip — unzip it somewhere clean and run it against a real board:

```bash
cd /tmp
unzip -o /path/to/fullwash-pcb-firmware/dist/FullWashInstaller.zip -d installer-test
cd installer-test
./install.command
```

Watch for `Done - you can unplug the board.` at the end. If something fails, check
`~/Library/Logs/FullWashInstaller.log` — that's the same file the README tells end users to send to
support.

## Updating `esptool` itself

`installer/bin/esptool` is a standalone PyInstaller-built binary (see `esptool.spec` at the repo
root) — a fallback/alternate path from the venv-based `esptool` that `install.command` installs on
the fly via `pip install esptool`. In practice `install.command` always installs the latest
`esptool` into its own venv at run time, so you normally don't need to touch
`installer/bin/esptool` or rebuild it. If you do need to rebuild that standalone binary (e.g. for an
offline installer variant), use PyInstaller with the existing spec:
```bash
pip install pyinstaller esptool
pyinstaller esptool.spec
```

## Limitations

- **macOS only.** `install.command` relies on macOS's "double-click to run" convention for `.command`
  files and on macOS-style `/dev/cu.*` serial device names. There is no Windows or Linux installer
  package today — those users need PlatformIO directly (see
  [docs/platformio-commands.md](platformio-commands.md)).
- The zip must be **fully unzipped** before running `install.command` — the script checks for all
  four `.bin` files next to it and refuses to run if any are missing (e.g. from a partial unzip).
