# Running PlatformIO (`pio`) Commands

This project is a [PlatformIO](https://platformio.org/) project (`platformio.ini` at the repo root),
targeting a single environment: **`T-SIM7600X`** (ESP32 + SIM7600 LTE modem board, `esp32dev` board
definition, Arduino framework, `espressif32@6.4.0` platform).

## Prerequisites

Install PlatformIO Core one of two ways:

- **VS Code extension** (recommended for day-to-day development): install the "PlatformIO IDE"
  extension. It installs PlatformIO Core under `~/.platformio/` and gives you build/upload/monitor
  buttons in the status bar, plus this repo already has `.vscode/extensions.json` recommending it.
- **Standalone CLI**: `pip install platformio`, or the
  [official installer script](https://docs.platformio.org/en/latest/core/installation/index.html).

Either way you end up with a `pio` executable. If the VS Code extension installed it and your shell
doesn't have it on `PATH`, call it directly:

```bash
~/.platformio/penv/bin/pio --version
```

Add `~/.platformio/penv/bin` to your shell `PATH` to just use `pio` everywhere (this doc assumes
`pio` is on `PATH` from here on).

## Everyday commands

Run these from the repo root (where `platformio.ini` lives).

**Build the firmware:**
```bash
pio run
```
There's only one environment defined (`T-SIM7600X`), so `pio run` and `pio run -e T-SIM7600X` are
equivalent. Build output goes to `.pio/build/T-SIM7600X/` — `firmware.bin`, `bootloader.bin`,
`partitions.bin`. This is also what `tools/package_installer.sh` looks for (see
[creating a package installer](building-package-installer.md)).

**Clean build artifacts:**
```bash
pio run -t clean
```
Use this if you suspect a stale build, or after changing `platformio.ini` build flags.

**Upload (flash) over USB:**
```bash
pio run -t upload
```
PlatformIO auto-detects the serial port. If you have multiple boards/USB devices attached, or
autodetection guesses wrong, target one explicitly:
```bash
pio run -t upload --upload-port /dev/cu.usbserial-XXXX   # macOS
pio run -t upload --upload-port /dev/ttyUSB0              # Linux
pio run -t upload --upload-port COM5                      # Windows
```

**List connected serial devices** (to find the port name above):
```bash
pio device list
```

**Open the serial monitor:**
```bash
pio device monitor
```
`monitor_speed` is already set to `115200` in `platformio.ini`, and `monitor_filters` includes
`esp32_exception_decoder`, so crash backtraces printed by the ESP32 get symbolicated automatically.
To watch boot output right after flashing, chain the two commands:
```bash
pio run -t upload && pio device monitor
```

**See all available build/upload targets:**
```bash
pio run --list-targets
```

**Verbose build** (full compiler/linker command lines — useful when debugging build flag issues):
```bash
pio run -v
```

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `pio: command not found` | `pio` isn't on `PATH` — call `~/.platformio/penv/bin/pio` directly, or add that dir to `PATH`. |
| `pio device list` shows nothing | Board isn't connected, or the USB-C cable is charge-only (common cause). Try a cable known to work for data. |
| Upload fails / times out | Wrong port selected, another program (e.g. Serial Monitor) has the port open, or the board needs to be put in bootloader mode manually depending on the board revision. |
| Build fails after pulling new changes | Run `pio run -t clean` then `pio run` again — stale `.pio/` state occasionally causes stale-library errors after `lib_deps` changes in `platformio.ini`. |
| Permission denied opening serial port (Linux) | Add your user to the `dialout` group: `sudo usermod -a -G dialout $USER`, then log out/in. |

## Where this fits in the release flow

`pio run -e T-SIM7600X` is also what `tools/package_installer.sh` runs (if needed) before it bundles
`.pio/build/T-SIM7600X/*.bin` into the end-user installer `.zip`. See
[docs/building-package-installer.md](building-package-installer.md) for that process.
