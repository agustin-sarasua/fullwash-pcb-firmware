# FullWash PCB — Schematic Description

Source: `docs/Document__1__34d8d0cc.pdf` (2 sheets, letter size).

## Overview

The board is a car-wash controller built around an **ESP32-WROVER-E-N16R8** module. It reads 6 user buttons and one electromechanical coin acceptor through an I2C I/O expander, drives 7 relays (12 V coils) for the wash functions, talks to a SIM7600G LTE modem, and has a display/RTC I2C bus, a micro-SD slot and a USB-C programming interface. Power comes from a 12 V input (screw terminal) and/or USB.

```
 12V in ──► +12V rail ──► relay coils, 12V sense divider
     │
     └──► 4.2V LDO ──► DVDD4V2 ──► ME6217C33 LDO ──► VCC3V3 (ESP32, TCA9535,
                                                     display, RTC, coin acceptor feed)

 ESP32 ── I2C bus A (IO18=SCL, IO19=SDA) ──► TCA9535 I/O expander (0x24)
       ── I2C bus B (IO22=SCL_LCD, IO21=SDA_LCD) ──► CH453S display driver, DS1340 RTC
       ── UART1 (IO26/IO27) ──► SIM7600G modem
```

## Sheet 1

### ESP32-WROVER-E (U1)
- Module: `ESP32-WROVER-E-N16R8` (16 MB flash / 8 MB PSRAM).
- Reset circuit: `EN` pulled up with R14 (10 kΩ) + C8 (1 µF), push-button to GND through R15 (22 Ω).
- Blue status LED (LED1) on **IO12** through R10 (1 kΩ).
- **IO35**: 12 V supply sense through a 300 kΩ / 100 kΩ divider (R13/R16) — ~3 V at 12 V input. (Input-only pin, ADC.)
- Decoupling: C6 (100 nF) + C7 (10 µF) on VCC3V3.
- UART0 (TXD0/RXD0) goes to the USB-to-UART block for programming.

### I2C I/O Expander (U2 — TCA9535RTWR)
- On I2C bus A: `SCL` = IO18, `SDA` = IO19, both with 10 kΩ pull-ups (R8, R11) to VCC3V3.
- `INT` (active-LOW, open-drain) → **IO23**, 10 kΩ pull-up (R12). *(The firmware no longer uses INT; it polls.)*
- Address pins strapped so the device answers at **0x24**.
- Decoupling: C1 (100 nF) + C2 (1 µF).

**Port 0 (inputs):**

| Pin | Net | Function |
|-----|-----|----------|
| P00 | BT6 | Button 6 (stop) |
| P01 | BT5 | Button 5 |
| P02 | BT4 | Button 4 |
| P03 | BT3 | Button 3 |
| P04 | BT2 | Button 2 |
| P05 | BT1 | Button 1 |
| P06 | COIN_SIG | Coin acceptor signal |
| P07 | — | Unused |

**Port 1 (outputs):** P10 = clear water, P11 = foam, P12 = vacuum, P13 = handwashing, P14 = inflatable, P15 = disinfect, P16 = lighting, P17 unused. Each drives a relay transistor.

> Note: `include/utilities.h` bit numbers and comments match this table (BUTTON1 = bit 5 = P05, COIN_SIG = bit 6 = P06).

### Buttons (BT1–BT6)
Each of the 6 buttons is identical:
- 100 kΩ pull-up to VCC3V3 (R1, R4, R7, R17, R20, R24).
- **100 nF capacitor to GND on the signal line** (C3, C4, C5, C9, C10, C13) — hardware RC debounce/noise filter (τ ≈ 10 ms).
- 2-pin 3.5 mm screw terminal (WJ15EDGRC-3.5-2P); the external button shorts the line to GND.
- **Active-LOW**: idle = 3.3 V, pressed = 0 V.

### Coin Acceptor (COIN1)
- 2-pin 3.5 mm screw terminal (WJ15EDGRC-3.5-2P):
  - **Pin 1 → VCC3V3** (the board's 3.3 V rail is fed out to the acceptor), decoupled by C12 (10 µF) at the connector.
  - **Pin 2 → COIN_SIG** net → TCA9535 P06.
- **R64 = 10 kΩ pull-DOWN** from COIN_SIG to GND.
- The electromechanical acceptor is just a switch: when a coin passes, the switch closes and connects 3.3 V to COIN_SIG.
- **Active-HIGH**: idle = 0 V (held by R64), coin present = 3.3 V. Confirmed on real hardware (2026-07-04) with a raw edge logger: idle reads LOW, coins produced single HIGH pulses of 80–130 ms.

Important asymmetries vs. the buttons (relevant to reliability, see the companion docs):
- Opposite polarity (buttons are active-LOW, coin is active-HIGH).
- **No filter capacitor on the signal line** (C12 sits on the 3.3 V feed, not on COIN_SIG).
- No series resistor, no Schmitt trigger, no TVS/ESD protection, no isolation — the raw expander pin, with a weak 10 kΩ pull-down, is wired straight to a cable leaving the board.

### Relays (RELAY1–RELAY7)
Seven identical channels:
- NPN transistor (Q1…Q7) with 1 kΩ base resistor and 10 kΩ base-emitter pull-down, driven by TCA9535 Port 1.
- Relay: **SRD-12VDC-SL-C**, coil from +12 V, switched by the transistor to GND.
- Flyback diode 1N4148WQ across each coil (D1…).
- The relay **NO contact** goes to a 2-pin screw terminal (CN1…CN7) that switches the external load (pump, vacuum, lights…).
- No snubber/varistor on the contact/load side.

### LCD (I2C) connector
- 4-pin screw terminal (WJ15EDGRC-3.5-4P): VCC3V3, GND, `SCL_LCD` (IO22), `SDA_LCD` (IO21).
- Firmware drives a CH453S 7-segment display controller (address 0x40) on this bus.

### RTC circuit
- **DS1340Z-33** RTC (U12) on the same LCD I2C bus (`SCL_LCD`/`SDA_LCD`), address 0x68.
- 32.768 kHz crystal (FC-135), 100 nF decoupling, VBACKUP to a battery connector (BAT).

### Micro SD Slot
- SD card socket wired to ESP32 (SPI-capable pins). Not used by current firmware.

### USB To UART
- USB-C connector (data) → USB-UART bridge, with the classic two-transistor auto-program circuit driving `EN`/`IO0` (DTR/RTS), so the board can be flashed without pressing buttons.

## Sheet 2

### SIM7600G modem
- UART: modem TXD → **IO26**, modem RXD ← **IO27** (through the level shifter).
- Control: `PWRKEY` ← **IO4**, `DTR` ← **IO32**, `FLIGHT` ← **IO25**, STATUS LED net.
- Antenna connectors for MAIN / GNSS / AUX, USB-C (data) also routed to the modem USB.
- Micro-SIM slot with ESD protection arrays.

### 1V8→3V3 level shifter
- Bidirectional level translator between the modem's 1.8 V logic and the ESP32's 3.3 V logic (UART + control lines).

### Power tree
- `Main_PWR` 2-pin screw terminal (12 V) → SS34A Schottky (D12, reverse protection) → C49 (22 µF) → **+12 V rail** (relays, sense divider).
- Two 4.2 V LDO/regulator stages: one from USB VBUS, one from the 12 V power jack; their outputs are OR-ed onto **DVDD4V2** via SS34A diode (D13) and NCE3401AY P-MOSFETs (Q9, Q10) that give the wired supply priority over USB.
- **ME6217C33M5G** LDO (U11): DVDD4V2 → **VCC3V3** (max ≈ 800 mA) — supplies the ESP32, expander, display, RTC and the coin-acceptor feed.
