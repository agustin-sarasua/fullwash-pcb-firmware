# Coin Acceptor — Electronics Recommendations for the Next Board Revision

The firmware can mask a lot (see `coin-acceptor-firmware-issues.md`), but the current input circuit is intrinsically fragile. This document explains why, and what to change.

## Why the current circuit is unreliable

Current topology (`COIN1` block, sheet 1):

```
VCC3V3 ──┬── C12 10µF ── GND
         │
         └──[cable]── coin switch ──[cable]── COIN_SIG ──► TCA9535 P06
                                                  │
                                                 R64 10kΩ
                                                  │
                                                 GND
```

Problems, in order of impact:

1. **High-impedance, unfiltered, active-HIGH node on an external cable.** COIN_SIG is held at 0 V only by a 10 kΩ pull-down and has **no capacitor, no series resistor, no Schmitt trigger**. The two conductors (3.3 V feed and signal) run side-by-side in the same cable, so their mutual capacitance couples every transient on the 3.3 V feed — and every EMI burst from the environment — straight into the pin. In a car-wash cabinet the environment includes relay contacts arcing while switching pumps and vacuum motors. Any coupled spike above the TCA9535's V_IH (~2.3 V) *is* a coin as far as the logic can tell. Compare with the buttons, which got this right: 100 kΩ pull-up **plus 100 nF filter cap** on every line.

2. **Essentially zero contact wetting current.** The mechanical switch contact carries 3.3 V / 10 kΩ = **0.33 mA**. Electromechanical contacts need a minimum current (typically 1–10 mA) to break through the oxide film that builds up on them; at 0.33 mA the film survives, contact resistance grows over time, and the divider (R_contact vs 10 kΩ) stops reaching the logic threshold → **coins pass and are not detected**, and it gets worse with age and humidity. This is the classic degradation mode of coin switches read at logic-level currents.

3. **The 3.3 V logic rail leaves the board.** A short in the acceptor cable, water ingress, or ESD on the coin slot couples directly into the rail that powers the ESP32, expander, display and RTC. No protection components at the connector.

4. **Polarity inconsistent with every other input.** Buttons idle HIGH / active LOW; the coin idles LOW / active HIGH. This asymmetry is what let the firmware polarity bug slip through.

5. **No transient suppression at the relay loads.** The relay COIL side has flyback diodes (good), but the CONTACT side switching external inductive loads (pumps, vacuum) has no RC snubber or varistor. Contact arcing there is the strongest EMI source on the machine and radiates into the coin cable. This aggravates issue 1.

## Recommended circuit — Option A (best): opto-isolated, 12 V sensing loop

Run the coin switch in a 12 V loop and isolate it completely from the logic:

```
+12V ──┬── R_a 2.2kΩ ── coin switch ──┬──►|── PC817 LED (with 1N4148 anti-parallel)
       │   (in acceptor loop)         │
       │                              └── R_b optional 10k to GND (bleed)

PC817 transistor side:
VCC3V3 ── R_c 10kΩ ──┬── COIN_SIG ──► expander/GPIO      (idles HIGH)
                     │
                  PC817 C-E ── GND                        (coin = LOW)
C_f 100nF from COIN_SIG to GND
```

Why this wins:

- **~5 mA wetting current** through the switch (12 V / 2.2 kΩ) keeps the contacts clean for years.
- **Galvanic isolation**: nothing that happens on the cable (ESD, shorts, coupled spikes) reaches the logic rail.
- Noise needs to deliver real current through the LED for the whole pulse to register — capacitively coupled spikes can't.
- The output side becomes **active-LOW with a pull-up and filter cap — identical topology to the buttons**, so hardware and firmware are consistent.
- BOM cost: one PC817 (or EL817/LTV-817), two resistors, one diode, one capacitor. Pennies.

## Recommended circuit — Option B (minimal change, stays at 3.3 V)

If you must keep the two-wire 3.3 V scheme, at least convert it to the button topology and condition the signal:

```
VCC3V3 ── R_pu 4.7kΩ ──┬── R_s 1kΩ ──► COIN_SIG (expander pin)
                       │                   │
                connector pin 2         C_f 100nF
                       │                   │
                 coin switch              GND
                       │
                connector pin 1 ── GND
```

- Switch now **shorts to GND** (active-LOW like the buttons) — the cable no longer carries the 3.3 V rail.
- 4.7 kΩ pull-up → ~0.7 mA wetting current (still low, but 2× today) and a stiffer idle level.
- R_s (1 kΩ) + C_f (100 nF) at the pin: RC filter (τ ≈ 100 µs) that kills coupled spikes while passing 20+ ms coin pulses untouched; R_s also limits ESD current into the pin.
- Add a **TVS diode** (e.g. PESD3V3L1BA) from the connector pin to GND.
- Optional but cheap insurance: a Schmitt buffer (74LVC1G17) between the RC and the expander pin for clean edges — the TCA9535 has no input hysteresis.

Firmware note: both options make the coin input **active-LOW**, which matches what most of the current code already (wrongly, today) assumes — but re-verify polarity against the new schematic when the code is fixed.

## Additional board-level recommendations

1. **Move COIN_SIG to a direct ESP32 GPIO** (e.g. IO13 or IO33, both free) instead of the I2C expander. A GPIO can use a hardware interrupt with microsecond latency and no I2C/mutex contention; the coin input is the most timing-sensitive signal on the board and currently the one read through the slowest path. Keep buttons on the expander.
2. **Snub the relay loads.** Put an RC snubber (e.g. 100 Ω + 100 nF X2, or a 275 V MOV for AC loads) across each relay contact / load at CN1–CN7. This attacks the phantom-coin problem at the source and extends relay contact life.
3. **Cabling**: use twisted pair (signal + return) or shielded cable for the coin acceptor run, keep it physically away from the pump/vacuum power wiring, and tie the shield to GND at the board end only.
4. **Protect all field connectors**, buttons included: series resistor + TVS per line costs little and this board lives in a wet, high-EMI cabinet.
5. **3.3 V budget**: the ME6217C33 (≈800 mA) feeds the ESP32 (WiFi/BLE bursts 400–500 mA), display, expander and RTC. It is workable but has little margin — brown-out dips shift input thresholds and cause exactly this kind of flaky behavior. For the next revision consider a small buck (e.g. AP63203/TPS62A02: 12 V → 3.3 V @ 2 A) instead of the LDO chain, or at least beef up bulk capacitance on VCC3V3.
6. **Keep the polarity documented on the schematic** (a text note "COIN_SIG: idle HIGH, coin = LOW, min pulse 20 ms" next to the connector). The current failure was ultimately a hardware/firmware contract mismatch — write the contract down where both sides can see it.

## Summary

| | Today | Option B (minimal) | Option A (opto) |
|---|---|---|---|
| Polarity | active-HIGH (unique on board) | active-LOW (matches buttons) | active-LOW (matches buttons) |
| Idle impedance | 10 kΩ pull-down | 4.7 kΩ pull-up | 10 kΩ pull-up |
| Signal filtering | none | RC + TVS (+ Schmitt) | opto + RC |
| Wetting current | 0.33 mA | ~0.7 mA | ~5 mA |
| Isolation | none (3.3 V rail on cable) | none (but GND-referenced) | full galvanic |
| Phantom-coin immunity | very poor | good | excellent |
| Missed-coin risk (aging contacts) | grows over time | reduced | essentially eliminated |
