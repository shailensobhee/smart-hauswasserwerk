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

### The six-line tap, probed

Six lines were brought out of the pump board and wired to D0-D5. A multimeter
read four at 3.35 V and two at 0 V, which fits an idle HD44780 bus just as well
as it fits a debug header — static levels cannot tell those apart. Edge
behaviour can, so `bus_probe` was written and flashed.

Across three separate boots, roughly 100 s of any-edge capture per boot, with
zero dropped samples:

| pin | level | edges/s | high % | min pulse | max high | max low | verdict |
|-----|-------|---------|--------|-----------|----------|---------|---------|
| D0  | 1     | 0       | 100 %  | —         | —        | —       | static  |
| D1  | 1     | 0       | 100 %  | —         | —        | —       | static  |
| D2  | 1     | 0       | 100 %  | —         | —        | —       | static  |
| D3  | 1     | 0       | 100 %  | —         | —        | —       | static  |
| D4  | —     | ~460    | 52 %   | 3 µs      | 10.5 ms  | 9.6 ms  | 50 Hz   |
| D5  | —     | ~2800   | 45 %   | 3 µs      | 8.9 ms   | 11.4 ms | 50 Hz   |

The coincidence matrix is **all zeros**: in ~150 000 captured edges, no two
pins ever changed in the same port read. There is no parallel bus here.

Then the decisive test, which needs no rewiring — reflash with the ESP32-C6's
internal ~45 kΩ pulldown engaged on all six pins. Any CMOS driver, and any
pull-up under ~10 kΩ, overrides that easily; a floating pin does not.

**Nothing changed.** Byte for byte the same picture.

- **D0-D3** are held high against the pulldown, so something under ~10 kΩ is
  holding them. But they never move — not one edge in 100 s.
- **D4 and D5** kept their behaviour, so the source is low impedance and real,
  not stray capacitive pickup.

And the periods settle what that source is. Longest high plus longest low is
the slowest full cycle each pin went through:

```
D4   10578 + 9623  = 20201 µs  ->  49.5 Hz
D5    8917 + 11417 = 20334 µs  ->  49.2 Hz
```

**20 ms is mains.** The 3 µs "pulses" are threshold chatter as a slow sine
crawls through the input trip point, ~100 crossings a second; the 52 % and
45 % duty figures are just two different DC offsets on the same 50 Hz swing.
`bus_probe` now detects this itself and prints `50Hz!` in place of the baud
guess, plus a warning block.

**Conclusion: there is no common ground between the pump board and the XIAO,
and the pump's reference floats at mains potential.** A node that follows the
line frequency is by definition not referenced to ours. That also explains
D0-D3: a pump-side node sitting above our 3V3 rail gets clamped there by the
ESP32's input protection diodes, which reads as an immovable logic high. It is
the clamp holding them, not a driver.

This retro-explains the −1.53 V measured on LCD header pin 0. A negative
reading like that is the signature of a floating reference, not a real negative
rail.

### The tap is blocked on isolation

No pin mapping and no decoder can fix a missing ground reference. The direct
tap cannot work at any wiring order, so the 24-permutation sweep described
below is moot until this is resolved. What comes next, in order:

1. **Disconnect the six wires from the XIAO.** Right now the only thing
   standing between the pump's floating reference and an earthed PC is the
   ESP32's ESD diodes, which are conducting.
2. **Measure it properly.** Pump live, ESP32 disconnected, AC volts from the
   pump's 0 V to protective earth **through a ~100 kΩ resistor** (an unloaded
   high-Z reading on a floating node means nothing). This is the test already
   listed as unresolved item 3 — the probe has now made answering it mandatory
   rather than merely prudent.
3. **If that reads near zero**, the supply is isolated: bond the pump's 0 V to
   the XIAO's `GND` pin — a ground wire, not a seventh GPIO — and re-run
   `probe.yaml`. Expect the static/50 Hz picture to collapse into real logic.
4. **If it reads tens or hundreds of volts**, the supply is a transformerless
   dropper. Then no direct connection is acceptable at any point, and the tap
   has to be galvanically isolated: an optocoupler per line, or a fully
   isolated front end, with the ESP32 never sharing a conductor with the pump.

### Unresolved — must be settled before wiring

1. **Which pin is which.** RS, E and the D4-D7 order are *not* determined. A
   multimeter cannot resolve this. Resolve by capture + software permutation
   (see [Solving the pin mapping](#solving-the-pin-mapping)), or with a ~EUR 10
   CY7C68013A logic analyser — PulseView ships an HD44780 decoder that renders
   a simulated display.

2. **Actual logic level.** The ~3.1-3.4 V DC readings on the signal lines are
   *time averages of a switching bus*, not logic levels. A 5 V line high 65 %
   of the time reads 3.25 V. VDD is 4.87 V and the panel controller is 5 V, so
   5 V CMOS signalling is likely. Corroborating tell: E is a narrow strobe and
   should average near 0 V, yet no signal pin does — consistent with either
   continuous refresh or 5 V levels.

   **ESP32-C6 GPIOs are not 5 V tolerant.** Until this is settled with a scope
   or logic analyser, put 10k/20k dividers or a 74LVC245 buffer on all six
   taps. That costs about EUR 1 and makes the question moot.

3. **Supply isolation.** *Now the blocking item — see
   [The tap is blocked on isolation](#the-tap-is-blocked-on-isolation). The bus
   probe found two tapped lines cycling at 50 Hz, which is direct evidence that
   the pump's reference is not ours.* Continuity from logic ground to Live,
   Neutral and Earth reads OL with the mains switch both off and on. That is
   the right test and a good sign, **but a capacitive-dropper or non-isolated
   buck supply reads OL too** and will still put logic ground at mains
   potential. The board has a relay but no obvious transformer, which points
   *towards* non-isolated. Before connecting a grounded PC:

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
- **All exposed pins are GPIO 0-23**, so a single `GPIO_IN_REG` read captures
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

### Tap pin assignment

The firmware defaults to a contiguous run on the XIAO silkscreen. Which LCD
header pin goes to which of these is not yet known — see
[Solving the pin mapping](#solving-the-pin-mapping).

| Role | XIAO pin | GPIO |
| --- | --- | --- |
| E | D0 | 0 |
| RS | D1 | 1 |
| D4 | D2 | 2 |
| D5 | D3 | 21 |
| D6 | D4 | 22 |
| D7 | D5 | 23 |

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

## The `hd44780_tap` component

Lives in [esphome/components/hd44780_tap/](esphome/components/hd44780_tap/) and
is pulled in as an ESPHome `external_components` local source. (ESPHome's old
`platform: custom` hook is deprecated and being removed; a local `components/`
directory is the current pattern.)

How it works:

- An `IRAM_ATTR` ISR fires on the **falling edge of E** — the instant the
  HD44780 latches RS and D4-D7. It does one `GPIO_IN_REG` read, grabs a
  microsecond timestamp, and pushes both into a lock-free ring buffer. Nothing
  else. Storing the whole port word rather than pre-extracted bits keeps the
  ISR to a single read and means changing the assumed pin roles only affects
  decoding, never capture.
- `loop()` drains the ring, pairs nibbles (high first), and runs a command
  state machine that maintains a shadow DDRAM.
- Rows 1 and 2 are published as text sensors.

Configuration:

| Option | Default | Purpose |
| --- | --- | --- |
| `e_pin`, `rs_pin`, `d4_pin`..`d7_pin` | required | The six taps. Must be distinct and GPIO0-31. |
| `columns` | `16` | Panel width. |
| `idle_gap` | `200us` | Gap that forces a byte boundary. |
| `raw_capture` | `false` | Log every edge and every decoded byte. |
| `line_1`, `line_2` | — | Text sensors for the rendered rows. |
| `edge_rate` | — | Diagnostic: observed E strobes per second. |
| `dropped_samples` | — | Diagnostic: edges lost to a full ring. |

Details the HD44780 makes easy to get wrong, and how they are handled:

- **Nibble framing.** 4-bit mode has no framing bits, so a decoder that joins
  mid-byte stays permanently off by one and still produces plausible-looking
  text. Two independent boundary signals recover from that: RS is held for a
  whole byte, so any RS change *must* land on a byte boundary; and an idle gap
  longer than `idle_gap` ends a byte. The second is the only signal available
  during a long run of same-RS character writes. Resyncs are logged at DEBUG.
- **`idle_gap` is a real trap.** A value below the true inter-nibble gap
  corrupts *every* byte. The 200 µs default is deliberately conservative;
  tighten it only after measuring actual gaps with `raw_capture: true`.
- **Set CGRAM Address (`0x40|n`).** The address counter is shared between DDRAM
  and CGRAM, so after this command every RS=1 write goes to the character
  generator, not the screen. The component ignores those writes until the next
  Set DDRAM Address; otherwise custom-glyph bytes smear across a row as fake
  text.
- **Cursor/display shift (`0x10-0x1F`).** Display shift moves the window, so a
  DDRAM address no longer maps to a fixed column. A shift offset is tracked and
  applied at render time. Cursor shift moves the address counter instead.
- **DDRAM is not contiguous.** `0x00-0x27` backs line 1, `0x40-0x67` backs
  line 2, and auto-increment runs `0x27 -> 0x40 -> ... -> 0x67 -> 0x00`. The
  visible 2x16 window is `0x00-0x0F` and `0x40-0x4F`.
- **Partial refreshes.** Firmware that rewrites only changed characters means
  the shadow buffer fills in over time. A cell that has never been written is
  *unknown*, not a space — it renders as `~`, and a row with nothing observed
  publishes nothing at all rather than an empty string.

### Solving the pin mapping

Six unknown pins is 720 permutations, but E and RS are identifiable from their
edge statistics, leaving 24 data orderings.

1. Set `raw_capture: true` and watch the logs. Enable `VERBOSE` in `logger:` to
   also see per-edge timing.
2. Confirm E first: it is the only pin whose edges the ISR triggers on at all,
   so a plausible `edge_rate` (rather than 0 or a flat-out storm) means E is
   right. RS follows from resync spam — a wrong RS pin produces constant
   "discarding orphan nibble" messages.
3. Sweep the 24 data orderings by editing the four `tap_d*_pin` substitutions in
   [esphome/hauswasserwerk.yaml](esphome/hauswasserwerk.yaml) and pushing OTA.
   Score against printable ASCII and the known strings `AutomaticMode`,
   `StandbyMode`, `CheckWater`, `ValveClosed`. No rewiring for any of it.

### Known risk in the ISR approach

The ISR reads the port a few microseconds *after* E falls, not at the edge
itself. The HD44780 only requires ~10 ns of data hold time, so a fast driver
could in principle change the data lines before the handler runs. In practice a
software-bitbang driver on a small MCU holds them for far longer, so this is
expected to work — but it is an assumption, not a guarantee.

The `edge_rate` and `dropped_samples` sensors exist to measure exactly this
before trusting any decode. ESP32 GPIO interrupt latency with WiFi active runs
~2-3 µs with real jitter; if the pump refreshes in a tight loop, edges will be
dropped and `dropped_samples` will climb. If it does, the fallback is the LP
core or a peripheral-based capture, not core affinity — the C6 has only one HP
core.

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

`secrets.yaml` is gitignored. Generate the API key with `esphome wizard` or any
base64 32-byte generator.

## Windows build environment

Two host requirements, both of which produce confusing failures if missed.

**1. Build from PowerShell, never Git Bash.** ESP-IDF's installer aborts with
`ERROR: MSys/Mingw is not supported` whenever `MSYSTEM` is present in the
environment (`idf_tools.py`, `if 'MSYSTEM' in os.environ`). Git Bash sets
`MSYSTEM=MINGW64`, and `env -u MSYSTEM` does **not** help: the MSYS2 runtime
re-injects the variable when it launches a native Windows binary such as
`python.exe`. Only a native parent process actually clears it.

**2. Long path support must be enabled.** ESP-IDF toolchain paths project to
~291 characters, past the 260-character `MAX_PATH` limit, which surfaces as
misleading errors like `fatal error: bits/c++config.h: No such file or
directory`. In an **elevated** PowerShell:

```powershell
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' LongPathsEnabled 1
```

## Usage

Run these from PowerShell, in the repository root:

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue

# Validate
.\.venv\Scripts\python.exe -m esphome config esphome\hauswasserwerk.yaml

# Build + flash over USB (first flash only)
.\.venv\Scripts\python.exe -m esphome run esphome\hauswasserwerk.yaml --device COM3

# Logs over the network, once WiFi is up
.\.venv\Scripts\python.exe -m esphome logs esphome\hauswasserwerk.yaml
```

After the first USB flash, updates go over the air — which is what makes the
24-permutation sweep cheap.

## Roadmap

1. **Resolve isolation and logic level.** Nothing else is safe to wire until
   these are settled. Fit dividers or a buffer regardless. The bus probe has
   turned this from a precaution into a hard blocker — see
   [The tap is blocked on isolation](#the-tap-is-blocked-on-isolation).
2. **External power monitoring.** A Shelly PM Mini Gen3 on the motor feed —
   metering-only, no relay contacts taking a 1300 W induction motor's inrush on
   every cycle. Fully external, zero risk, and gives the ground truth every
   later stage checks against.
3. **Button injection + auto-rearm**, with the cold-boot guard and the
   `CheckWater` interlock from [Safety interlocks](#safety-interlocks).
4. **LCD sniffer.** ✅ Firmware written — see
   [the component](#the-hd44780_tap-component). Not yet wired or validated
   against real traffic.
5. **Bus probe.** ✅ Written, flashed, and it has already earned its keep: it
   proved the six-line tap has no valid ground reference before a single byte
   was misdecoded. See [The six-line tap, probed](#the-six-line-tap-probed).
6. *Optional:* inline 0-10 bar pressure transducer and a YF-B10 brass Hall flow
   sensor, read via ADS1115. A continuous pressure curve is more diagnostic
   than any discrete display state — it shows bladder pre-charge degradation,
   slow leaks, and dry-run onset before the MCU latches.

## Status

Hardware reverse-engineering is complete and measured. The LCD sniffer firmware
is written and compiles, but **has never seen a real bus** — every decode path
in it is untested against actual traffic, and the pin roles it assumes are
placeholders.

A six-line tap was wired to D0-D5 and probed. It carries no data as connected:
four lines are immovably clamped high and two are cycling at 50 Hz, which is
measured proof that the pump's reference floats with respect to the ESP32's.
**Those wires should come off the XIAO until isolation is settled** — see
[The tap is blocked on isolation](#the-tap-is-blocked-on-isolation). Unresolved
items 2 and 3 (logic level, supply isolation) now block everything downstream,
and item 3 is no longer a theoretical concern.

Superseded: an earlier UART probe firmware, written before the bus measurements
existed. There is no serial line on this board, so it could never have produced
data. Its connectivity, antenna, logging and diagnostics sections were carried
over into the current firmware.
