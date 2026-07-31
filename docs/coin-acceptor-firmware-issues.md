# Coin Acceptor — Firmware Issues (keeping the current PCB)

> **Status: FIXED (2026-07-04).** The state machine described at the bottom of this
> document is now implemented in `TaskCoinDetector` (`src/main.cpp`), together with
> items 1–7 of the recommendations. The issue descriptions below refer to the code
> as it was before that change and are kept as the rationale.
>
> **Field-verified (2026-07-04).** A raw edge logger on real hardware confirmed the
> hardware contract below: COIN_SIG idles LOW and each coin produces a single clean
> HIGH pulse — two test coins measured **130 ms and 80 ms** wide, well inside the
> 20–500 ms acceptance window. (First data points for item 8's width distribution.)

This document explains why coin detection is unreliable **at the firmware level**, assuming the electronics stay exactly as they are. Read `hardware-schematic-description.md` first for the circuit.

## The hardware contract (from the schematic)

- COIN_SIG (TCA9535 P06) has a **10 kΩ pull-down (R64)** and the coin switch connects it to **3.3 V**.
- Therefore: **idle = LOW (0), coin present = HIGH (1)**. The signal is **active-HIGH**.
- A passing coin produces one HIGH pulse, typically tens of ms wide (depends on coin speed), with contact bounce on both edges. Measured on real hardware (2026-07-04): 80–130 ms, no visible bounce at 5 ms polling.

## Issue 1 (root cause): inverted polarity in `TaskCoinDetector`

`src/main.cpp:143-163`:

```cpp
// LOW = coin present (active), HIGH = no coin      <-- WRONG for this PCB
bool coinLow = ((portVal & (1 << COIN_SIG)) == 0);

if (coinLow) {
    lowReads++;
    if (lowReads >= COIN_STABLE_READS_REQUIRED && !triggered) {
        ... ioExpander.setCoinSignal(1);  // "COIN DETECTED"
} else {
    lowReads = 0;
    triggered = false;   // latch re-armed by ANY single HIGH read
}
```

The task assumes the line idles HIGH and a coin pulls it LOW. On this PCB it is the exact opposite. What actually happens at runtime:

1. At boot the line idles LOW → the `triggered` latch stays armed → no detection while idle (looks fine).
2. A coin closes the switch → line goes HIGH → the code takes the `else` branch: `triggered = false` (latch disarmed).
3. The coin passes, the line falls back to LOW → after 2 polls (`COIN_STABLE_READS_REQUIRED` × 5 ms = 10 ms) → **"COIN DETECTED"**.

So the detector *works by accident as a trailing-edge detector*: it never validates the coin pulse itself, it fires on the **return to idle**. Every failure mode below follows from this.

### Consequence A — phantom coins ("coins appear out of nowhere")

A **single** HIGH sample is enough to disarm the latch (`triggered = false`), and the idle level (LOW) is what is being "validated" as the coin. So any noise spike on COIN_SIG that is caught by even one 5 ms poll produces a coin event 10 ms later. The line is a weakly held (10 kΩ) unfiltered input wired to an external cable next to relay-switched motor loads — spikes are guaranteed. The 2-consecutive-reads filter protects the wrong edge: it filters the idle level (always stable) instead of the pulse.

### Consequence B — phantom coins at startup (your "especially at the beginning")

`src/main.cpp:114-127`: after `COIN_STARTUP_DELAY` (3 s) the task takes **one single sample** to decide the latch state:

```cpp
bool initialLow = ((pv & (1 << COIN_SIG)) == 0);
triggered = initialLow;   // LOW → armed, HIGH → "ready"
```

Boot time is the electrically noisiest moment (3.3 V rail settling, SIM7600 power bursts, relays initializing, cable capacitance charging because the acceptor's 3.3 V feed powers up with the board). If that one sample — or any sample shortly after — catches the line HIGH, the latch disarms and the very next two idle-LOW reads register an immediate coin. That is exactly the "ghost coin right after power-up" symptom.

### Consequence C — missed coins

- The task only "sees" the coin if at least one poll lands inside the HIGH pulse. Polling is 5 ms (`COIN_POLL_INTERVAL_MS`), but each cycle first competes for `xIoExpanderMutex` with a 10 ms timeout and **skips the sample entirely on failure** (`src/main.cpp:135-141`). `TaskButtonDetector` takes the same mutex every 10 ms, and the controller/relay code takes it too (a `setRelay` does 3 I2C transactions while holding it). A short pulse from a fast coin can fall entirely into skipped polls.
- `COIN_COOLDOWN_MS = 800` is applied twice (task `src/main.cpp:152` **and** controller `src/car_wash_controller.cpp:1022`). Two coins inserted quickly, or a coin right after a phantom event, get eaten by the cooldown.
- Hardware-side marginality (dirty switch contact vs. the 10 kΩ pull-down; see the electronics doc) can keep the pulse below the expander's input threshold — no firmware fix can recover those.

## Issue 2: contradictory polarity across the codebase

Three places disagree about what "coin present" means — clear evidence the polarity was never pinned down against the schematic:

| Place | Assumed polarity |
|-------|------------------|
| `src/main.cpp:143` (`TaskCoinDetector`) | coin = LOW |
| `src/io_expander.cpp:303-318` (`handleInterrupt`, now dead code) | coin = HIGH (bit set = 3.3 V) |
| `src/car_wash_controller.cpp:49-56` (init comments/logs) | "When coin is present: Pin is LOW" |

The interrupt handler had it right; the polling task that replaced it has it inverted.

## Issue 3: `COIN_MIN_PULSE_WIDTH_MS` is defined but never used

`include/constants.h:41` declares a 30 ms minimum pulse width "to filter noise spikes", but no code references it. There is currently **no pulse-width qualification at all** — the single most effective software defense against phantom coins on this hardware is not implemented.

## Issue 4: I2C read errors return `0x00` and are consumed as data

`src/io_expander.cpp:71,77` — `readRegister()` returns `0` on any I2C error. Callers can't distinguish "error" from "port reads 0x00":

- For `TaskButtonDetector`, `0x00` means **all six buttons pressed at once** (buttons are active-LOW) — a bus glitch can fire spurious button events.
- For a *correctly written* (active-HIGH) coin detector, an error read would look like "no coin", truncating a real pulse; for the current inverted logic it counts toward "coin present" reads.

The API needs an explicit success flag so error samples are discarded, not interpreted.

## Issue 5: startup windows are duplicated and drift apart

The task has its own 3 s startup gate (`src/main.cpp:110-131`) and `handleCoinAcceptor()` has a second, independent one that starts on its **first call** (`src/car_wash_controller.cpp:981-1003`). A flag set by the task while the controller is still in its own window is not cleared — it sits latched and is processed the moment the controller window expires, so a boot-noise event can surface as a coin *seconds later*.

## Recommended firmware fix (single detector, correct polarity, pulse qualification)

Replace the body of `TaskCoinDetector` with a state machine that validates the HIGH pulse:

```cpp
// Constants (constants.h)
COIN_POLL_INTERVAL_MS   = 5;    // keep
COIN_MIN_PULSE_MS       = 20;   // >= 4 consecutive HIGH samples
COIN_MAX_PULSE_MS       = 500;  // longer than this = stuck switch / miswire, not a coin
COIN_IDLE_REARM_MS      = 50;   // line must be LOW this long before next coin can start
COIN_STARTUP_QUIET_MS   = 1000; // line must be continuously LOW this long after boot to arm

// Pseudo-code
state IDLE:      // armed, line LOW
    if sample == HIGH -> pulseStart = now; state = IN_PULSE
state IN_PULSE:  // measuring the HIGH pulse
    if sample == HIGH and (now - pulseStart) > COIN_MAX_PULSE_MS -> state = FAULT (log, wait for LOW)
    if sample == LOW:
        width = now - pulseStart
        if width >= COIN_MIN_PULSE_MS -> registerCoin()
        else -> discard (noise spike)
        state = REARM; lowSince = now
state REARM:     // ignore break-bounce
    if sample == HIGH -> state = IN_PULSE (bounce continues; extend)
    if sample == LOW and (now - lowSince) >= COIN_IDLE_REARM_MS -> state = IDLE
```

Details that matter:

1. **Polarity**: coin = bit set (HIGH). Fix the comments in `car_wash_controller.cpp` init at the same time.
2. **Arm only after a quiet period**: after `COIN_STARTUP_DELAY`, require the line continuously LOW for `COIN_STARTUP_QUIET_MS` before entering `IDLE`. Never trust a single sample (fixes the boot ghosts).
3. **Qualify the pulse width** (20–500 ms). A 5 ms poll gives 4+ samples inside a real coin pulse; single-sample spikes are discarded. This finally uses (a renamed) `COIN_MIN_PULSE_WIDTH_MS`.
4. **Make `readRegister` failures explicit** (e.g. `bool readRegister(uint8_t reg, uint8_t &value)`); on error, skip the sample without advancing the state machine. Fixes the button issue too.
5. **One cooldown, one owner**: keep the cooldown in the task only (measured from pulse end), delete the duplicate in `handleCoinAcceptor`, and remove the controller's second startup window (the task already gates startup).
6. **Reduce sampling jitter**: raise the coin task priority above `TaskButtonDetector` (e.g. 2 vs 1) and consider `Wire.setClock(400000)` so mutex hold times shrink; log a counter of skipped samples (mutex timeouts) so starvation is visible in the field.
7. Delete the dead interrupt path (`handleInterrupt`, `enableInterrupt` call in `setup()`) or bring it back deliberately — but don't keep two half-alive detection paths with opposite polarities.
8. Keep an event log: on every accepted/rejected pulse, log the measured width. After a week of field data you will know the real pulse-width distribution of your acceptor and can tighten the window.
