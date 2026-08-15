# smart-hauswasserwerk

Monitoring and power-cut recovery for a **Parkside PHGA 1300 A1** domestic
waterworks pump, using a Seeed Studio XIAO ESP32C6 running ESPHome and
reporting into Home Assistant.

The ESP32 **passively taps the pump's internal LCD bus** to read its display,
and injects button presses in parallel with the existing tactile switches. It
never drives the LCD bus.

## The problem

After a power cut the pump returns to `StandbyMode` and does not resume
`AutomaticMode` by itself. Someone has to walk to the cellar and press MODE.
This is a known complaint in the Parkside community.

Power-on state is deterministic (`StandbyMode`) and Automatic is one MODE press
away, so the core fix needs no display decoding at all — but see
[Safety interlocks](#safety-interlocks), because a naive `on_boot` press is
genuinely dangerous.

## The device

| Property | Value |
| --- | --- |
| Model | Parkside PHGA 1300 A1 "Haus- und Gartenautomat" |
| OEM | Scheppach GmbH |
| IAN | 502965_2504 |
| Rating | 1300 W, 230 V, max 5 bar, 5000 l/h, G1" ports |

Two internal PCBs: a power board (mains + motor relay) and a display/control
board (MCU, 2x16 character LCD on a 10-pin header, 3 tactile buttons).

### Operating modes

| Mode | Behaviour |
| --- | --- |
| `StandbyMode` | Entered whenever the pump is plugged in. Deterministic power-on state. |
| `AutomaticMode` | Pumps on demand. Stops after 10 s of no flow, starts when pressure drops. |
| `TimeMode` | Timer window once per day. **Dry-run protection is NOT active.** |
| `AlwaysOn` | Uncontrolled continuous run. **Dry-run protection is NOT active.** |

### Display messages (LCD line 2)

| Text | Meaning |
| --- | --- |
| `PUMPOFF` | Pumping interrupted; resume with ON/OFF |
| `CheckWater` | Dry-run protection tripped |
| `P` / `L` | P = pressure built, L = water flowing |
| `ValveClosed` | All outlets closed, no flow |
| `NOTSET` | Timer mode, no times configured |
| `12:34:56` | Timer mode active / current time |

### Dry-run protection

Stops after 30 s with no water. Retries after 20 s for another 30 s. After two
failed starts it **latches off permanently** and shows `CheckWater`; recovery
requires a physical CHECK button press. Do not automate around this.

## Measured hardware findings

All values below were measured with a multimeter on the real board, not
inferred from datasheets.

### The 10-pin LCD header

Pins numbered 0-9 left to right, viewed from above with the display facing you.

| Pin | DC (V) | Identification | Tap? |
| --- | --- | --- | --- |
| 0 | **-1.53** | V0 / VEE contrast bias | **NEVER — negative rail will destroy a GPIO** |
| 1 | +4.87 | VDD (+5 V) | No |
| 2 | 3.23 | Logic signal | Yes |
| 3 | 3.33 | Logic signal | Yes |
| 4 | 0 | VSS / GND | Yes — ground reference |
| 5 | 3.34 | Logic signal | Yes |
| 6 | 0 | RW, strapped low | Not needed |
| 7 | 3.08 | Logic signal | Yes |
| 8 | 3.20 | Logic signal | Yes |
| 9 | 3.34 | Logic signal | Yes |

Pin 4 reads 0 V DC *and* 0 V AC, so it is genuinely static — ground. Pin 6
reads 0 V DC and 3.4 mV AC, also static — RW tied low.

### Interface: HD44780, 4-bit, write-only

Six active logic lines (pins 2, 3, 5, 7, 8, 9) = `RS, E, D4, D5, D6, D7`.
Textbook 4-bit HD44780.

**RW tied low means the bus is write-only.** The MCU never reads the busy flag,
the LCD never drives the data lines, and passive tapping carries zero
contention risk. The negative bias rail confirms the panel has its own
controller IC rather than raw multiplexed glass.

### Unresolved — must be settled before wiring

1. **Which pin is which.** RS, E and the D4-D7 order are *not* determined. A
   multimeter cannot resolve this. Resolve by capture + software permutation
   (see [Roadmap](#roadmap)), or with a ~EUR 10 CY7C68013A logic analyser —
   PulseView ships an HD44780 decoder that renders a simulated display.

2. **Actual logic level.** The ~3.1-3.4 V DC readings on the signal lines are
   *time averages of a switching bus*, not logic levels. A 5 V line high 65 %
   of the time reads 3.25 V. VDD is 4.87 V and the panel controller is 5 V, so
   5 V CMOS signalling is likely. Corroborating tell: E is a narrow strobe and
   should average near 0 V, yet no signal pin does — consistent with either
   continuous refresh or 5 V levels.

   **ESP32-C6 GPIOs are not 5 V tolerant.** Until this is settled with a scope
   or logic analyser, put 10k/20k dividers or a 74LVC245 buffer on all six
   taps. That costs about EUR 1 and makes the question moot.

3. **Supply isolation.** Continuity from logic ground to Live, Neutral and
   Earth reads OL with the mains switch both off and on. That is the right
   test and a good sign, **but a capacitive-dropper or non-isolated buck supply
   reads OL too** and will still put logic ground at mains potential. The board
   has a relay but no obvious transformer, which points *towards*
   non-isolated. Before connecting a grounded PC:

   - Inspect the topology: isolated flyback = small transformer + optocoupler +
     a visible isolation slot in the PCB. Non-isolated = X2 cap in series with
     Live, or an inductor-only buck with no barrier.
   - With the pump live, measure AC volts from logic ground to protective earth
     **through a ~100 kohm resistor**. Unloaded high-Z readings on a floating
     node are meaningless.
   - Until settled, treat the logic section as live and use optocouplers.

4. **Button topology.** Confirm each button pad reads to ground with the pump
   unplugged. Three buttons on one ADC pin via a resistor ladder is a common
   cheap-board pattern, and a MOSFET-to-ground would register the wrong button
   or short the ladder.

> AC voltage readings from a handheld DMM are not used for any conclusion here.
> Handheld meters spec AC accuracy over roughly 45-400 Hz; on kHz digital edges
> the readings are out of band and unreliable.

## Controller board

| Property | Value |
| --- | --- |
| Board | Seeed Studio XIAO ESP32C6 |
| Chip | ESP32-C6FH4 (QFN32), rev v0.2 |
| Flash | 4 MB embedded |
| Radios | Wi-Fi 6, BT 5 (LE), IEEE 802.15.4 |
| USB | Native USB-Serial/JTAG (no CP2102/CH340 bridge) |
| MAC | `58:E6:C5:1A:9D:98` |
| Port | `COM3` |

### Pin map (silkscreen -> GPIO)

| Pin | GPIO | | Pin | GPIO |
| --- | --- | --- | --- | --- |
| D0 | 0 | | D6 | 16 (TX) |
| D1 | 1 | | D7 | 17 (RX) |
| D2 | 2 | | D8 | 19 (SCK) |
| D3 | 21 | | D9 | 20 (MISO) |
| D4 | 22 (SDA) | | D10 | 18 (MOSI) |
| D5 | 23 (SCL) | | LED | 15 |

GPIO3 and GPIO14 control the antenna RF switch; both are driven LOW at boot to
select the on-board ceramic antenna.

### Consequences of this board choice

- **GPIO budget is tight but sufficient.** 6 LCD taps + 3 button injectors = 9
  of the 11 exposed pins.
- **All exposed pins are GPIO 0-23**, so a single `GPIO.in.val` read captures
  every tap atomically. No register tearing.
- **No strapping-pin conflicts.** The C6 straps on GPIO 8, 9 and 15; none of
  D0-D10 map to those.
- **Single HP core.** The C6 has one 160 MHz RISC-V application core plus a
  20 MHz LP core, so the usual "pin the sniffer to the second core, away from
  WiFi" trick is unavailable. If the E strobe turns out to be fast, the LP core
  or a peripheral-based capture is the fallback, not core affinity.

## Wiring

**Listen-only on the LCD bus.** The six taps are configured as inputs and never
driven.

- Use **100-330 ohm series resistors at each tap** *for signal integrity*, keep
  stubs under ~10 cm, and run a ground wire alongside the bundle. Six probe
  wires on a parallel bus add real capacitance and can corrupt the pump's own
  display. If the display starts glitching after connecting, this is why.
- Those series resistors are **not** GPIO overvoltage protection. See
  unresolved item 2 — use dividers or a buffer for that.
- Never connect pin 0 (-1.53 V) or pin 1 (+5 V) to a GPIO.
- Button injection: one 2N7002 N-channel MOSFET per button, gate to GPIO, drain
  to the button's signal side, source to ground, wired **in parallel with the
  existing switch** so the physical buttons keep working. Add a 100k gate
  pulldown on each, or ESP32 boot/reset float will phantom-press. Pulse
  ~100-120 ms. PC817 optocouplers are a valid alternative and are *required* if
  isolation is not confirmed.

## Safety interlocks

These are load-bearing, not nice-to-haves.

**Only auto-press MODE on a true cold boot.** `on_boot` also fires on OTA
updates, crashes, brownouts and watchdog resets. If the pump is already in
Automatic, a spurious press advances it to TimeMode or `AlwaysOn` — which has
**no dry-run protection**. Gate the press on
`esp_reset_reason() == ESP_RST_POWERON`, and prefer powering the ESP32 from the
pump's own supply so the two share one power-cycle event.

**Never auto-rearm after `CheckWater`.** A power cycle clears the pump's
latched dry-run lockout. Without an interlock the sequence *well runs dry ->
pump latches CheckWater -> power blips -> firmware presses MODE* restarts the
pump into a still-dry well, defeating the protection the latch exists to
provide. Persist the last observed `CheckWater` to NVS and refuse to auto-rearm
until a human clears it.

Note this makes the "no decoding required" auto-rearm depend on *some* state
source — either the LCD sniffer or an external power meter.

**Independent ground truth beats assumed state.** Open-loop button pressing
desyncs from reality. A power meter on the motor feed (0 W idle vs
~900-1300 W pumping) is more reliable than the display, because a pump that
only rewrites changed characters may never let the sniffer establish full
state after an ESP32 restart.

**Mains safety.** The power board's primary side carries live mains regardless
of the logic section's isolation. Keep probes off it, prefer working unplugged,
and make sure the circuit is RCD/FI protected. An independent leak sensor plus
a normally-closed motorised valve is worth more for flood protection than
anything in this repo.

## Setup

The toolchain lives in a project-local virtualenv, so nothing is installed
globally beyond Python itself.

```bash
python -m venv .venv
./.venv/Scripts/python.exe -m pip install esphome
```

Copy the secrets template and fill it in:

```bash
cp esphome/secrets.yaml.example esphome/secrets.yaml
```

`secrets.yaml` is gitignored. Generate the API key with
`esphome wizard` or any base64 32-byte generator.

## Usage

```bash
# Validate
./.venv/Scripts/python.exe -m esphome config esphome/hauswasserwerk.yaml

# Build + flash over USB (first flash only)
./.venv/Scripts/python.exe -m esphome run esphome/hauswasserwerk.yaml --device COM3

# Logs over the network, once WiFi is up
./.venv/Scripts/python.exe -m esphome logs esphome/hauswasserwerk.yaml
```

After the first USB flash, updates go over the air.

## Roadmap

1. **Resolve isolation and logic level.** Nothing else is safe to wire until
   these are settled. Fit dividers or a buffer regardless.
2. **External power monitoring.** A Shelly PM Mini Gen3 on the motor feed —
   metering-only, no relay contacts taking a 1300 W induction motor's inrush on
   every cycle. Fully external, zero risk, and gives the ground truth every
   later stage checks against.
3. **Button injection + auto-rearm**, with the cold-boot guard and the
   `CheckWater` interlock from [Safety interlocks](#safety-interlocks).
4. **LCD sniffer.** ISR on the falling edge of E (when HD44780 latches), an
   `IRAM_ATTR` handler doing nothing but one atomic register read and a
   ring-buffer push, nibble reassembly in the main loop (high nibble first),
   and a shadow 2x16 buffer exposed as two text sensors.
5. *Optional:* inline 0-10 bar pressure transducer and a YF-B10 brass Hall flow
   sensor, read via ADS1115. A continuous pressure curve is more diagnostic
   than any discrete display state — it shows bladder pre-charge degradation,
   slow leaks, and dry-run onset before the MCU latches.

### Sniffer implementation notes

- **Nibble framing.** 4-bit HD44780 has no framing bits, so starting mid-stream
  can leave the decoder permanently off by one nibble, producing plausible
  garbage. Resync using RS: **a change in RS must fall on a byte boundary.**
- **Solve the pin mapping in software, not by hand.** 6 pins is 720
  permutations, but E and RS are identifiable from their edge statistics,
  leaving 24 data orderings. Add a raw-capture mode that streams timestamped
  6-bit samples, then score all 24 offline against printable ASCII and the
  known strings (`AutomaticMode`, `StandbyMode`, `CheckWater`, `ValveClosed`).
  Keep the pin `#define`s in one block at the top of the header so a manual
  override stays trivial.
- **Measure the E period before committing to the ISR design.** ESP32 GPIO ISR
  latency with WiFi active is ~2-3 us with real jitter. If the pump refreshes
  in a tight loop, edges will be dropped.
- **The command state machine needs more than the obvious opcodes.** Handle
  `0x40|n` Set CGRAM address — after it, RS=1 writes go to CGRAM, not the
  display, and custom characters would otherwise land in the buffer as fake
  text. Also handle `0x10-0x1F` cursor/display shift, which breaks the
  address-to-row mapping, and DDRAM auto-increment wrap
  (`0x0F -> 0x10..0x27 -> 0x40`).
- **Partial refreshes.** Some firmware rewrites only changed characters, so the
  shadow buffer fills in over time. Treat a blank buffer as *unknown*, never as
  a real state.
- **Use `external_components:`, not `platform: custom`.** ESPHome's custom
  component platform is deprecated and being removed; a local `components/`
  directory is the current pattern for a C++ component exposing text sensors.

## Status

Hardware reverse-engineering is complete and measured. No sniffer or control
firmware is written yet.

`esphome/hauswasserwerk.yaml` currently contains a **UART probe** written
before the bus measurements existed. There is no serial line on this board, so
that firmware cannot produce data and is superseded. Its connectivity, antenna,
logging and diagnostics sections are still a good base to build the real
firmware on.

Row mapping for the 2x16 panel, for reference: DDRAM `0x00-0x0F` = line 1,
`0x40-0x4F` = line 2.
