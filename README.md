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

### Interface: two-wire clocked serial, 9-bit words

**Measured and decoded on 2026-09-18.** The display is driven over exactly two
of those lines:

| Property | Value |
| --- | --- |
| Clock | XIAO D1, data valid on the **rising** edge, idles high with ~11 µs low pulses |
| Data | XIAO D2 |
| Word | **9 bits — one flag bit, then 8 data bits MSB-first** |
| Line marker | payload `7C 40`, then 16 character words |
| Frame | ~10 frames/s, 382 bits; **191 bits when only one line is drawn** |
| Phase | line 2 starts **two bits out of phase** with line 1 |

The 9-bit word is the whole story of why this took so long to read. Under an
8-bit assumption every ninth bit shifts the stream, and the result is not a
degraded decode — it is nothing at all, at every alignment. See
[The 10-pin LCD header, swept](#the-10-pin-lcd-header-swept).

The leading word group (`F8 00 00`) and the inter-line group
(`3E 40 60 1F 10`) are not identified. They are skipped, not decoded.

> **This is not an HD44780 parallel bus**, and an earlier version of this README
> said it was. Six logic-level readings on the header were read as
> `RS, E, D4-D7` — a reasonable inference from static DC voltages, and wrong.
> Only two of the ten lines ever move. The [`hd44780_tap`](#the-hd44780_tap-component)
> component and the DDRAM model in `hd44780_core` are correct code for panels
> that *do* use that bus; they are simply not what this pump needs.

**RW tied low still means write-only.** The MCU never reads back, the panel
never drives, and passive tapping carries zero contention risk. The negative
bias rail confirms the panel has its own controller IC rather than raw
multiplexed glass.

### Display protocol, decoded

Rendered live by [`clocked_tap`](#the-clocked_tap-component). Screens observed
so far:

```
[  Standby Mode  ][                ]
[ Automatic Mode ][                ]
[ Automatic Mode ]                      <- 191-bit frame, no second line at all
[   Time Mode    ][    NOT SET     ]
[Time Now   00:00][Save  Set   Exit]
[   Always On    ][Not Recommended ]    <- line 2 blinks at 1 Hz
```

Two behaviours that any consumer of this data has to handle:

- **A blank line 2 and an absent line 2 are different states.** The panel
  either sends 16 spaces or truncates the frame to 191 bits. Anything assuming
  a fixed frame length breaks on the second case.
- **`Not Recommended` blinks**, 500 ms on, 500 ms off, indefinitely. The two
  half-frames alternate between fixed byte patterns of identical length, so
  this is the panel and not a decode artefact. Debouncing cannot suppress it —
  the blink *is* stable. Downstream logic must hold the last non-blank line or
  treat the alternation as one state.

### Button mapping

MODE walks a four-position cycle. Every leg measured on 2026-09-18 with
`live.yaml` streaming:

```
Standby Mode  ->  Automatic Mode  ->  Time Mode  ->  Always On  ->  Standby Mode
```

**One press is not one screen change.** Pressing MODE into Time Mode produced
three logged screens from a single press:

| Δt | Screen | Cause |
| --- | --- | --- |
| +0.0 s | `[   Time Mode    ][                ]` | the button |
| +0.8 s | `[Time Now   00:00][Save  Set   Exit]` | menu opens by itself |
| +21 s | `[   Time Mode    ][    NOT SET     ]` | menu times out by itself |

Attributing the later two to presses would be wrong, and they arrive slowly
enough to look like separate presses. The run log in
[esphome/live.yaml](esphome/live.yaml) carries the full sequence with
timestamps.

## Two different headers

Everything above this line describes the **10-pin LCD header**: measured,
identified, HD44780 4-bit with RW strapped low. That analysis stands.

Everything below describes a **separate 6-pin header** found elsewhere on the
same board. None of the LCD header's conclusions transfer to it. Do not read
"six lines" in either section as referring to the other.

Both have been swept. The LCD header came back with display text; the 6-pin
header came back empty, and is now being re-swept — see below.

| | 10-pin LCD header | 6-pin unknown header |
| --- | --- | --- |
| Identified? | Yes — decoded and rendering live | No — swept once, carried nothing |
| Protocol | [Two-wire clocked serial, 9-bit words](#interface-two-wire-clocked-serial-9-bit-words) | Unknown; no runtime traffic seen |
| Wired to the XIAO? | Only the two signal lines, soldered to D1/D2 | Yes — all six, on D3, D4, D5, D6, D9, D10 |
| Tooling | [`bus_sweep`](#the-bus_sweep-component), then [`clocked_tap`](#the-clocked_tap-component) | [`bus_sweep`](#the-bus_sweep-component), plus GPIO binary sensors in HA |

**Both are connected at once now**, which they never were before, and that is
the point of the current run. The two known display lines act as a *reference
pair* inside the same capture, so the six unknowns can be compared against them
directly by the [net-identity test](#the-net-identity-test-are-two-of-these-the-same-wire).

### Why re-sweep a header that already came back empty

The display tap on D1/D2 works, but those two wires are **soldered to the
display**. The 6-pin connector needs no soldering. If the clock or the data
line is among those six, the soldered wires come off and
[`clocked_tap`](#the-clocked_tap-component) moves to the easy pins — which is
worth one flash to find out.

Set against that: runs 1-3 swept a 6-pin header and found every line static over
a full 60 s, which points at *no*. Three things make it worth re-testing anyway,
rather than treating the old negative as settled:

1. **Different physical connection.** The six are on different XIAO pins now.
   Runs 1 and 2 of that header were ruined by a loose wire, so a fresh
   connection is not a formality here.
2. **A reference pair in the same capture.** The old runs had six unknowns and
   nothing to compare them to. Any line matching D1 or D2 now announces itself.
3. **The net-identity test did not exist.** It is the measurement that can
   actually answer the question, and no previous run had it.

If it comes back static again, that is a real answer reached in one flash, and
the soldered tap stays.

### The 6-pin header, probed

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

**Conclusion: there was no common ground between the pump board and the XIAO.**
A node that follows the line frequency is by definition not referenced to ours.
That also explains D0-D3: a pump-side node sitting above our 3V3 rail gets
clamped there by the ESP32's input protection diodes, which reads as an
immovable logic high. It is the clamp holding them, not a driver.

This retro-explains the −1.53 V measured on LCD header pin 0. A negative
reading like that is the signature of a floating reference, not a real negative
rail.

What it does *not* establish is *why* the reference floated — see
[Isolation: resolved](#isolation-resolved). And under the "unknown header"
framing, four permanently static lines is no longer a puzzle: **that is exactly
what an idle ISP/SWD/ICSP programming header looks like.** Such headers carry
VCC, GND, RESET and two or three programming pins, and they are silent unless a
programmer is attached. If that is what this is, there is nothing here to sniff
and the LCD header is the real target.

### Isolation: resolved

The logic section is decoupled from mains. The 50 Hz result above was not
evidence of a non-isolated supply — it was an **isolated secondary that was not
bonded to anything**, floating on the transformer's interwinding capacitance.
On a meter and on a probe capture those two cases look identical, which is
exactly why the earlier conclusion could not distinguish them.

A ground bond now exists, via the LCD backlight cathode.

> **That bond needs one continuity check before it can be trusted.** Many panels
> dim by switching the backlight *cathode* low-side through a transistor. Such a
> node reads ~0 V while the backlight is lit and looks exactly like ground, but
> it is a switching node, not a reference. With the pump unplugged, check
> continuity from the cathode pad to LCD header pin 4 (VSS). If it does not
> beep, move the bond to pin 4 before capturing anything. `bus_sweep` prints a
> mains-coupling verdict that points back at this check.

All six voltages in this README predate the bond and were taken against a
floating reference — which is why LCD pin 0 read −1.53 V. **Re-measure against
the bonded ground before wiring anything.** That is also the damage check:
anything above 3.3 V must come off the XIAO, because ESP32-C6 GPIOs are not
5 V tolerant.

Re-running [`probe.yaml`](esphome/probe.yaml) is the gate. If the `50Hz!`
markers are gone, the bond took and the capture is meaningful. If they are
still there, it did not, and nothing downstream means anything.

### The 6-pin header, swept

With the bond in place and the D4 wire reseated, `bus_sweep` ran a full 60 s
capture on 2026-09-18:

| pin | level | edges | ISR calls | polled changes |
| --- | --- | --- | --- | --- |
| D0-D3 | 1 (high) | 0 | 0 | 0 |
| D4-D5 | 0 (low) | 0 | 0 | 0 |

**Four lines strapped high, two strapped low, nothing moving for a minute.**

D5 is the result that validates the bond: it was cycling at 50 Hz before, and
is now pinned low and silent. That is the only direct evidence available that
the backlight cathode really is ground.

All three counters agreeing at zero is what makes the negative trustworthy. The
interrupt path and the independent polled cross-check both saw nothing, so this
is a real silence and not a capture that failed — a distinction this
investigation had to learn the hard way:

> Two earlier runs were ruined by a **loose D4 wire**. Intermittent contact
> produced ~310 edges/s of threshold chatter — 772 gaps at 4-8 µs, nothing
> between 16 µs and 2 ms — which filled the entire 4096-sample buffer in 4.6 s
> and starved every other line. Its 100 % solitary-edge fraction (D4 never
> changed in the same port read as another line) was the tell. Between those,
> one run captured nothing at all while `bus_probe` saw thousands of edges
> minutes later, which looked like a firmware bug and was not. **If chatter
> like that appears, check the connection before believing any of it.**

The `CAPTURE BROKEN` verdict and the polled cross-check were built in response
to that scare and are worth keeping regardless: a false negative here ends the
investigation, so it must never be the quiet outcome.

**Remaining caveat:** no run so far has had the pump's buttons worked during
the capture window. An event-driven header is indistinguishable from a dead one
until provoked, and the `Re-run Sweep` button exists for exactly that.

> **Caveat since closed.** A human cannot press a button inside a window that
> shuts 0.58 s after arming, so the firmware now presses MODE itself from
> inside the capture — see [the self-provoked
> sweep](#the-idle-capture-blind-spot-and-the-self-provoked-sweep). Provoked
> runs did not change the answer: see [the 6-pin header,
> answered](#the-6-pin-header-answered).

**The next step is the meter, not more firmware.** With every line static and a
valid ground reference, DC volts on each of the six identifies rails directly,
and no amount of capture can add to that. Four pulled-up lines plus two grounds
is the shape of a programming header — AVR ISP is MISO/VCC/SCK/MOSI/RESET/GND,
PIC ICSP is VPP/VDD/VSS/PGD/PGC/NC.

> **Superseded as a conclusion, not as evidence.** These six are now on D3, D4,
> D5, D6, D9 and D10 and are being re-swept alongside the known display lines —
> see [why](#why-re-sweep-a-header-that-already-came-back-empty). The reading
> above is still the best guess going in, and the meter is still owed.

### The 6-pin header, answered

**The clock and the data are not among the six.** The soldered D1/D2 tap is the
only way to read this display, and it stays. Settled 2026-09-20 by runs 15 and
16 in [sweep.yaml](esphome/sweep.yaml), whose inline log carries the detail.

| pin | what it is |
| --- | --- |
| D3, D4 | floating — 50 Hz mains pickup, 100 % solitary. **Untested, not tested-negative**; reseat before reading anything into them |
| D5, D9, D10 | static HIGH in every capture, idle and provoked alike |
| D6 | the only one of the six carrying signal — frame-synchronous, responds to MODE, uncorrelated with the display bus |

Run 9 had already answered this, but from a *silence*: D6 logged zero edges
while the clock pushed 5256 edge/s, and a line on the clock's net cannot be
quiet while the clock is talking. That argument went stale when D6 later started
free-running at 6000–8000 edge/s with no firmware change to the pin — a silent
input is an untested one, exactly as this README already says of D3/D4. Run 16
put D1-CLK, D2-DAT and D6 in one buffer for the first time:

```
level disagreement (% of 16383 samples, moving lines only):
         D1-CLK D2-DAT     D6           and 60 s later:
D1-CLK        -   1.3%  88.4%           D1-CLK  -  0.5%  86.5%
D6        88.4%  88.5%      -           D6   86.5%  86.7%     -
No two moving lines share a net.

best hypothesis with D6 as clock:  0.5  (the real bus, D1/D2, scored 22.5)
```

88.4 % is not merely "neither 0 nor 100". With D1 high 98.5 % of the time and D6
high 11.1 %, *independence* predicts 0.985×0.889 + 0.015×0.111 = **87.7 %**. The
lines are uncorrelated, not just un-shorted. And the duty cycles settle it
without reference to the capture at all: an inverted copy of a line that is high
98.5 % of the time must be high 1.5 % of the time, and D6 is high 11.1 %.
Inversion is arithmetically impossible however long the capture runs — which
closes the loophole the earlier polarity argument left open, since opposite idle
levels rule out a *shared* net but not an *inverted* one.

**What is left is a meter job.** DC volts on all six against the bonded ground,
pump unplugged. No amount of capture identifies a line that is static in every
run.

#### Three lessons the last two runs cost

**A provoked sweep needs a press witness.** Run 14 dropped D6 from the tap
because it was flooding the buffer, then reported `Active lines: (none)` under
MODE press — which reads as "these lines do not respond" and equally as "the
press never arrived", and nothing in the report distinguishes them. D6 was the
only line known to answer the stimulus, so removing it removed the control.
Restoring it produced `max_hi 119336 µs` against a commanded 120 ms press, the
fourth such reproduction. *Never run the provoked sweep without a line known to
respond to the stimulus.*

**Microsecond blips on a static line are crosstalk, not a response.** D5, D9 and
D10 do produce edges during a press, which for several runs looked like the
event-driven keypad header this project was hoping for. The shape says
otherwise: both instants shared across all three lines to the microsecond
(50259 µs, then 178935/178933 µs), pulses 2–5 µs wide on lines otherwise
100.0 % HIGH, aligned with contact-close and contact-open. Simultaneous
microsecond transients on three separate conductors at a switching event are
capacitive coupling. A real response looks like D6: 119 ms, on one line,
100 % solitary.

**`OTA successful` does not tell you which image is running.** Run 16's first
attempt uploaded, rebooted and printed a complete report — of the *previous*
firmware. The six-pin image browned out during boot before ESPHome could mark it
valid, and the bootloader rolled back:

```
[I][app:151]: ESPHome version 2026.7.4 compiled on 2026-09-20 01:29:45
[W][safe_mode:094]: OTA rollback detected! Rolled back from partition 'app1'
[W][safe_mode:094]:  The device reset before the boot was marked successful
[W][safe_mode:099]: Last reset was due to brownout - check your power supply!
```

A second identical OTA booted clean. **Check the boot banner's `compiled on`
timestamp against the build you just made** — the only symptom otherwise is a
report that disagrees with your config, which invites exactly the wrong
diagnosis.

The brownout itself is a standing hazard, not a fluke: `bus_sweep` attaches its
ISR in `setup()` and gates only the *storing* of samples, so every tapped edge
runs the handler from boot onward. Four pins with one busy line boots fine; six
pins with three busy lines roughly triples that load while WiFi is associating.
The earlier eight-pin runs were on USB power. On the external supply the margin
is gone.

### The 10-pin LCD header, swept

All ten LCD header lines were wired to XIAO D0-D9 and swept on 2026-09-18.
**The display text came out.**

```
VERDICT: CLOCKED  CLK=D1/rise  DATA=D2  MSB-first  align2  (165 B)
decoded: ...@@@  |..|@  Standby Mode  |..|@   @@@...|@  Standby Mode  >@`>
```

`Standby Mode` is the pump's own display string, decoded twice inside a single
0.4 s capture and reproduced across three independent captures at the same
clock/data pair. A twelve-character contiguous phrase is not something a wrong
hypothesis produces by chance.

> **The pins, edge and bit order above are right. `align2 (165 B)` is not, and
> the reason it is wrong is not fully explained.**
>
> The bus uses **9-bit words**. This sweep only tried 8-bit ones, so it could
> not read the bus — and that is not a matter of degree. Replaying a captured
> 382-bit frame through the sweep's own decode logic:
>
> | word length | result |
> | --- | --- |
> | 8 bits, all 8 alignments | nothing — no known string, on one frame or on eight concatenated |
> | 9 bits, align 0 | `\|@  Standby Mode  >@\`` |
>
> Frame-level analysis agrees: under an 8-bit reading, **zero** hits across 61
> unique frames × 8 alignments × both bit orders, best printable fraction
> 18/47, only 100 ones in 376 bits. Under 9-bit words the same frames render
> `Time Mode`, `Standby Mode`, `NOT SET`, `Always On` and `Not Recommended`
> exactly.
>
> **So where did the original `Standby Mode` come from?** Unknown. The obvious
> guess — that an 8-bit read drifts through a lucky phase — was tested and is
> false; 382 is not a multiple of 8 or 9, so frame boundaries shift the phase,
> and 8-bit still never spells it. A plausible untested explanation is that
> that capture was missing roughly one edge in nine (the buffer filled in 0.4 s
> with D7/D8 noise), which would make the stored stream *effectively* 8 bits
> per word. The original raw capture was not kept, so this cannot be settled.
> It is recorded as unexplained rather than given a tidy story.
>
> `bus_sweep` now sweeps 8- and 9-bit words and prints the word length in its
> verdict. The lesson worth keeping: **a hypothesis family with a hidden fixed
> parameter fails by scoring well for the wrong reason, not by scoring badly.**
> A low score prompts doubt; a plausible verdict carrying a real fragment does
> not.

| pin | edges | edge/s | high % | solo % | reading |
| --- | --- | --- | --- | --- | --- |
| D0 | 0 | 0 | 100 % | — | static HIGH |
| **D1** | 2657 | 6648 | 96.0 % | 76 % | **clock** — idles high, 11 µs low pulses |
| **D2** | 474 | 1186 | 92.9 % | 12 % | **data** — 399 edges coincident with D1 |
| D3 | 0 | 0 | 0 % | — | static LOW |
| D4 | 0 | 0 | 100 % | — | static HIGH |
| D5 | 0 | 0 | 0 % | — | static LOW |
| D6 | 0 | 0 | 100 % | — | static HIGH |
| D7 | 1335 | 3340 | 18.9 % | 87 % | noise — solitary, nothing decodes |
| D8 | 5258 | 13157 | 55.9 % | 96 % | noise — locked to 50 Hz mains |
| D9 | 0 | 0 | 0 % | — | static LOW |

The solitary-edge column separates the signal from the noise cleanly. D2 moves
without D1 only 12 % of the time; D7 and D8 move alone 87 % and 96 % of the
time, which is a floating input, not a bus.

**What this does not establish:** *which LCD header position* D1 and D2 are.
The sweep labels XIAO silkscreen and the wiring order into the header was never
written down. That is a meter job and is still outstanding.

The six static lines were read at the time as possibly four loose wires.
**Resolved, and it was the other branch:** this panel takes a serial interface,
two wires carry the entire display, and the rest are rails and unused pins.
Nothing needs re-seating. The loose-D4 episode was good reason to check first,
but it was not the answer this time.

HD44780 scored nothing here because it was never tried — the hypothesis needs
six moving lines and only four moved, so its permutation count was zero. That
was read as a statement about the tap rather than a rejection of HD44780. Fair
at the time; but the tap was fine and HD44780 is simply not what this panel
speaks.

> **Two of these wires must come off.** The +4.87 V supply and the negative
> contrast rail are still in the tap. They are among the static lines: the
> HIGHs are {D0, D4, D6} and the LOWs are {D3, D5, D9}. ESP32-C6 GPIOs are not
> 5 V tolerant and have no tolerance at all for a negative rail — both are
> clamping through the protection diodes right now.

### Unresolved — must be settled before wiring

1. **Which pin is which.** ~~On the LCD header, RS, E and the D4-D7 order are
   not determined.~~ Resolved as far as decoding goes — clock is D1, data is
   D2, and the display renders. What is still missing is the *physical* map
   from XIAO D0-D9 to LCD header positions 1-10, which is a meter job, not a
   firmware one. Needed to reproduce the wiring, not to read the bus.

2. **Actual logic level.** The ~3.1-3.4 V DC readings on the signal lines are
   *time averages of a switching bus*, not logic levels. A 5 V line high 65 %
   of the time reads 3.25 V. VDD is 4.87 V and the panel controller is 5 V, so
   5 V CMOS signalling is likely. Corroborating tell: E is a narrow strobe and
   should average near 0 V, yet no signal pin does — consistent with either
   continuous refresh or 5 V levels.

   **ESP32-C6 GPIOs are not 5 V tolerant.** Until this is settled with a scope
   or logic analyser, put a **74LVC245 buffer powered from 3V3** on all six
   taps. Its inputs are 5 V tolerant and its V<sub>IH</sub> is 2.0 V, so it
   accepts either level. A resistor divider does *not* work here: 10k/20k turns
   a 3.3 V bus into 2.2 V, which is below the C6's V<sub>IH</sub> of roughly
   0.75 × VDD ≈ 2.5 V. One part, about EUR 1, and the question is moot at
   either logic level.

3. ~~**Supply isolation.**~~ **Resolved** — the logic section is decoupled from
   mains and a ground bond is in place. See
   [Isolation: resolved](#isolation-resolved) for the one continuity check that
   still has to pass before the bond can be trusted.

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

**Listen-only on the LCD bus.** Every tap is configured as an input and never
driven.

The working tap is **two wires** — clock and data — not six. The six-tap
parallel wiring below belongs to the HD44780 model this panel turned out not to
use; it is kept for reference and for other panels.

- Use **100-330 ohm series resistors at each tap** *for signal integrity*, keep
  stubs under ~10 cm, and run a ground wire alongside the bundle. Probe wires
  add real capacitance and can corrupt the pump's own display. If the display
  starts glitching after connecting, this is why. Less of a concern on a
  two-wire tap than on a six-wire parallel one, but the clock here runs at
  ~6.6 kedges/s and still deserves a short stub.
- Those series resistors are **not** GPIO overvoltage protection. See
  unresolved item 2 — use a 74LVC245 for that, not a divider.
- Never connect LCD pin 0 (the contrast bias rail) or pin 1 (+5 V) to a GPIO.
- Bond the pump's 0 V to the XIAO's `GND` pin. A ground wire, not a seventh
  GPIO.
- Button injection: **no parts needed for MODE.** The switch was metered and
  one of its two terminals sits at 0 V, so an open-drain GPIO on the other
  terminal reproduces the press on its own — see
  [MODE injection](#mode-injection-no-parts-required). The MOSFET below remains
  the pattern for any button whose terminals both turn out to be live.
- Button injection, general case: one 2N7002 N-channel MOSFET per button, gate
  to GPIO, drain to the button's signal side, source to ground, wired **in
  parallel with the existing switch** so the physical buttons keep working. Add
  a 100k gate pulldown on each, or ESP32 boot/reset float will phantom-press.
  Pulse ~100-120 ms. PC817 optocouplers are a valid alternative and are
  *required* if isolation is not confirmed. If *neither* terminal is ground, a
  MOSFET is the wrong part — its body diode conducts one way with the gate off,
  so it never opens cleanly; use a photoMOS SSR (TLP222A, AQY212GH) or a reed
  relay, which are genuinely bidirectional.

### Tap pin assignment

**In use** — [live.yaml](esphome/live.yaml), the two lines that carry the
display:

| Role | XIAO pin | GPIO | Source | Direction |
| --- | --- | --- | --- | --- |
| Clock (rising) | D1 | 1 | soldered to display | input only |
| Data | D2 | 2 | soldered to display | input only |
| MODE inject | D7 | 17 | MODE switch | **output**, open-drain |
| MODE inject | D8 | 19 | MODE switch | **output**, open-drain |
| Unknown | D3 | 21 | 6-pin header | input only |
| Unknown | D4 | 22 | 6-pin header | input only |
| Unknown | D5 | 23 | 6-pin header | input only |
| Unknown | D6 | 16 | 6-pin header | input — but see below |
| Unknown | D9 | 20 | 6-pin header | input only |
| Unknown | D10 | 18 | 6-pin header | input only |

Everything except D7 and D8 is listen-only. Those two are the first pins in this
project that are driven at all, and they are open-drain for that reason.

D0 (GPIO0) is deliberately free — it is the spare if the D6 wire has to move.

> **D6 is GPIO16, the ESP32-C6's UART0 TX.** The mask-ROM bootloader prints its
> banner on that pin at 115200 baud at **every reset**, before any firmware
> runs. Nothing in ESPHome can suppress it: `common.yaml` logs over
> USB_SERIAL_JTAG, so UART0 is unclaimed once booted and the pin sits as a
> plain input — but the boot burst has been going into that pump-board line on
> every power-up and every OTA since the wire went on.
>
> It is harmless at 3.3 V into a logic input, and it is long over before any
> capture starts. It is still the ESP32 talking to the pump board uninvited. If
> the pump ever behaves oddly right after an ESP reset, this is the first
> suspect, and the fix is to move that wire to **D0**.

Which LCD header position the clock and data sit on is still unrecorded — see
[unresolved item 1](#unresolved--must-be-settled-before-wiring).

### MODE injection, no parts required

The MODE button is an ordinary momentary switch and **one of its two terminals
is at 0 V**. That single measured fact is what removes the hardware from this:
"short D7 to D8" and "pull the 3.3 V terminal down to 0 V" are the same
electrical event, and a GPIO can do the second one unaided.

Without a grounded terminal it could not have. A GPIO connects a line to one of
its own rails; it cannot connect two lines to each other. Shorting two live
nets needs a real switch across them — a photoMOS SSR or a reed relay, both
bidirectional and isolated. That part is not needed here.

**Open-drain is the safety property, not a style choice.** An open-drain pin
can only sink. It can never push 3.3 V onto a line the pump's MCU is driving,
so the worst case if a wire is on the wrong pad is a pin held low rather than
two push-pull drivers fighting each other. It also means sinking the grounded
terminal costs nothing — it is already at 0 V.

**Both pins are driven together**, because which of the two is the 3.3 V side
was never recorded, and the sweep could not tell: it read D7 and D8 as noise,
87 % and 96 % solitary edges, which is exactly what an open switch looks like
from a high-impedance input. Pulling the grounded one to ground is a no-op, so
driving the pair is correct without knowing which is which. Meter it and the
other line can be dropped from the script.

Press current returns through the existing 0 V bond rather than directly
between the two pads. It is pull-up current — sub-milliamp.

Pulse length is 120 ms, per the 100–120 ms in [Wiring](#wiring). The script is
`mode: single`, so a double-click in the HA UI drops the second run instead of
stacking presses or leaving the line held low by one run while another releases
it. It then holds itself busy for a further 400 ms, because the panel is not
finished when the contact opens — a press into Time Mode produces three screens
over 21 s as its menu opens and times out.

There is no `on_boot` press, and the pins are plain high-Z inputs until ESPHome
configures them, so boot, OTA and watchdog resets cannot phantom-press. That
matters: `on_boot` fires on all three, and a spurious press can advance the
pump into `AlwaysOn`, which has **no dry-run protection** — see
[Safety interlocks](#safety-interlocks).

**Not in use** — the HD44780 assignment in
[hauswasserwerk.yaml](esphome/hauswasserwerk.yaml), for a bus this panel does
not have. Kept for other panels; see
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

> **Not the component this pump needs.** The panel turned out to speak
> [two-wire clocked serial](#interface-two-wire-clocked-serial-9-bit-words), not
> 4-bit parallel — see [`clocked_tap`](#the-clocked_tap-component). This one has
> never decoded a byte off real hardware. It is kept because it is correct code
> for panels that *do* use an HD44780 bus, and because its DDRAM model is shared
> with `bus_sweep` via `hd44780_core`. The description below is unchanged and
> still accurate about HD44780; it is just aimed at a bus this pump does not
> have.

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

Six unknown pins is 720 permutations. The cheap way to resolve them is
[`bus_sweep`](#the-bus_sweep-component), which scores all 720 against a single
capture in one flash. The manual route — confirm E and RS from edge statistics,
then push 24 OTA builds — still works and is described under
[`raw_capture`](#the-hd44780_tap-component), but there is no longer a reason to
prefer it.

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

## The `clocked_tap` component

Lives in [esphome/components/clocked_tap/](esphome/components/clocked_tap/) and
is driven by [esphome/live.yaml](esphome/live.yaml). **This is the component
that reads this pump.** It decodes the two-wire serial bus described in
[Interface](#interface-two-wire-clocked-serial-9-bit-words) continuously and
publishes the two display rows.

Where `bus_sweep` answers *"what is on these lines?"* in one 0.4 s shot, this
answers *"what does the panel say right now"* and keeps answering. That
difference is not cosmetic: the sweep's buffer fills faster than a human can
reach the front panel, so **a button press essentially never lands inside its
window.** No amount of timing the press better fixes that.

How it works:

- An `IRAM_ATTR` ISR on the configured clock edge does one `GPIO_IN_REG` read
  plus a microsecond timestamp into a lock-free ring. Same shape as the other
  components here, same reason.
- `loop()` drains the ring under a bounded budget, so WiFi, API and OTA are
  never starved on the C6's single HP core.
- Frames end on an idle gap. The measured bus leaves three decades of daylight
  to sit in — intra-frame gaps top out near 64 µs, frames are ~90 ms apart — so
  `frame_gap` is not a delicate setting.
- Decoding is **marker-driven, not offset-driven**, and the frame is held as a
  *bit* string rather than bytes.

Design details that are load-bearing:

- **The scan hunts the `7C 40` marker at every bit position.** Line 2 starts two
  bits out of phase with line 1, so there is no fixed alignment that renders
  both — the decoder has to be free to resync anywhere. Resyncing on content
  also contains damage: a slipped clock edge costs the line it lands in rather
  than everything after it.
- **Payload is the last eight bits of each word**, whatever `word_bits` is, so a
  9-bit flagged stream and a plain 8-bit stream share one code path.
- **Repeat suppression keys on the decoded text, not the raw bits.** Raw dedup
  suppresses nothing: single-bit corruption made 61 consecutive frames unique
  in one capture while all of them were the same two lines.
- **`stable_frames` debounce.** A flipped bit renders as a one-cell difference
  that is otherwise indistinguishable from a real change, and would log a
  spurious screen every time. Requiring two identical consecutive decodes costs
  100 ms at 10 frames/s and removed every glitch: a 2.5-minute run logged
  `0 dropped, 0 undecoded, 23 garbled cells` and **not one** false change line.
- **Unchanged screens re-print every 10 s.** Otherwise an attached log looks
  dead, and silence has to mean "no traffic" rather than "nothing new".

Configuration:

| Option | Default | Purpose |
| --- | --- | --- |
| `clock_pin`, `data_pin` | required | The two taps. Must differ and be GPIO0-31. |
| `edge` | `rising` | Which clock edge samples data. |
| `bit_order` | `msb_first` | Within the 8 payload bits. |
| `frame_gap` | `1ms` | Idle gap that ends a frame. |
| `word_bits` | `9` | Word length. Set `8` for a stream with no per-word flag. |
| `line_marker` | `[0x7C, 0x40]` | The two payload bytes introducing a display line. |
| `columns` | `16` | Character words read after a marker. |
| `stable_frames` | `2` | Identical decodes required before a change is believed. |
| `blank_hold` | `1500ms` | How long a line must stay blank before the blank is published. |
| `log_repeats` | `false` | Log every frame, including repeats. |
| `line_1`, `line_2` | — | Text sensors for the rendered rows. |

`line_marker` is not guessable — it was read off a captured frame as the only
thing appearing exactly once per line. On a different panel, dump the hex at
DEBUG and look for what repeats.

### What is published is not what is logged

Deliberately. The log prints the panel verbatim, all sixteen columns of it,
because protocol work has to see reality. The text sensors get the opposite
treatment, for two reasons that only show up once the data has a consumer:

- **Values are trimmed.** The panel centres its text, so almost every screen
  arrives padded — `"  Standby Mode  "`. Home Assistant templates should not
  have to know that.
- **Blanks are held for `blank_hold`.** The Always On screen blinks its warning
  at 1 Hz, forever, and that blink is *real* — `stable_frames` cannot touch it,
  because it is the panel's genuine state and it is perfectly stable. Published
  straight through it would write two state changes a second into HA's recorder
  for as long as the pump sits in that mode. So a line going blank has to stay
  blank longer than the blink half-period before the blank is published. A true
  blank — Standby Mode's second line — still gets through, 1.5 s late.

A line the frame did not carry at all publishes blank too, rather than holding
its last value. The panel truncates the frame to 191 bits instead of sending
sixteen spaces, so without that a one-line screen would leave the previous
screen's second line standing indefinitely.

There is intentionally **no "Pump Mode" entity**. Line 1 is not always a mode —
on the Time screen it reads `Time Now   00:00`. Mapping text to a mode is a
guess about panel behaviour, and it belongs in a Home Assistant template where
it can be corrected without a reflash.

## The `bus_sweep` component

Lives in [esphome/components/bus_sweep/](esphome/components/bus_sweep/) and is
driven by [esphome/sweep.yaml](esphome/sweep.yaml). Where `bus_probe` answers
*"is anything happening on these lines"*, `bus_sweep` answers *"what protocol is
it, and which line is which"* — without rewiring and without one flash per
guess.

**Capture once, replay many.** The ISR stores whole-port words with timestamps
into one flat buffer — role-agnostic, no interpretation at capture time. Every
hypothesis is then scored against *that same buffer*. Nothing that separates two
hypotheses can be an artefact of one run happening to be busier than the other,
which is the flaw in sweeping by reflashing.

Three hypothesis families, all replayed over the same data:

| Family | Space searched | Primary score |
| --- | --- | --- |
| HD44780 | every ordered `(E, RS, D4-D7)` — 720 for six lines | fraction of Set DDRAM Address targets landing in `0x00-0x27` / `0x40-0x67` |
| UART | each line as TX × 8 standard bauds, 8N1 | printable-ASCII fraction minus framing-error rate |
| Clocked | each clock/data pair × both polarities × both bit orders × word lengths {8, 9} × every bit alignment | best combined score over all alignments |

The DDRAM-address test is the strongest single discriminator available. A wrong
pin assignment scatters addresses roughly uniformly over `0x00-0x7F`, so about a
quarter of them land in the `0x28-0x3F` / `0x68-0x7F` hole that no real
controller ever addresses. A correct assignment essentially never does. Scores
also weight substring hits against the [known display
strings](#display-messages-lcd-line-2), and penalise orphan nibbles.

The clocked family is deliberately weighted lowest. It has the most free
parameters, so it has the most opportunity to fit noise by coincidence.

Also reported: a per-pin table with duty, pulse extremes and a **solitary-edge
fraction** (a strobe or clock changes alone; data lines cluster), a log2 **gap
histogram** per pin (bimodal gaps are the signature of framed data), and the
coincidence matrix.

Analysis is chunked to a ~10 ms budget per `loop()` so WiFi, API and OTA are
never starved.

### The net-identity test: are two of these the same wire?

Not a hypothesis — a direct measurement, and the only part of the report that
can be *proved* rather than scored.

Two pins on one physical net must read the same level in **every** sample,
because a sample is a single atomic `GPIO_IN_REG` read. There is no skew, no
propagation delay and no sampling window to explain a difference away. So the
sweep counts, for each pair of moving lines, the samples in which their levels
disagreed:

| Disagreement | Reading |
| --- | --- |
| 0 over thousands of samples | **same wire** |
| < 0.1 % | same wire, with glitches — check the connection |
| ~100 % | an **inverted** copy, through a buffer or transistor |
| anything else | independent lines |

**This is strictly stronger than the coincidence matrix**, which is why both are
printed. Coincident edges only say two lines move together, and every
synchronous bus does that by construction — the measured clock and data share
399 coincident edges and are plainly different wires. Coincidence is
suggestive; level agreement is decisive.

The findings are emitted at `WARN` so they stand out in a report that is
otherwise a wall of `INFO`, and the pin pair is prepended to the published
`Sweep Verdict`:

```
SAME WIRE: D3 == D1-CLK - 0 disagreements in 16383 samples.
INVERTED:  D9 is an inverted copy of D2-DAT (99.9% disagreement).
```

Two limits worth knowing. It only runs over lines that **moved**: a line sharing
a net with a live signal cannot be static, and two *static* lines are
indistinguishable to this test no matter how long the capture runs — that case
belongs to the meter. And fewer than 64 samples produces no finding at all,
because two unrelated lines agree by chance over a handful of reads.

#### The duty-cycle guard, and the wrong answer that forced it

As first written the test produced a confident false positive on run 10c:

```
SAME WIRE: D5 == D10 - 0 disagreements in 16383 samples.
```

D5 and D10 are not the same wire. They were event lines at rest, each HIGH for
100.0 % of the capture, and **two unconnected wires that both sit high always
agree**. The same run called D6 an inverted copy of three separate lines, for
the mirror-image reason.

The "only lines that moved" filter was supposed to prevent exactly this and
cannot: it asks whether a line moved *at all*, and eight edges in 16 384 samples
is a line that is static 99.95 % of the time. **An edge guard is the wrong
guard.** What the test needs is for each line to have *spent time at both
levels*, because

```
min(samples_high, compared - samples_high)
```

is the number of samples that could have disagreed — the real denominator of the
evidence, as opposed to the nominal one. Both lines in a pair must clear 64, since
a shared net has one duty cycle by definition and one busy line does not license
a claim about a static partner. Pairs that fail are reported rather than skipped:

```
6 pair(s) too static to test: a line needs >= 64 samples at its
minority level before agreement means anything.
```

"No pair was testable" and "no pair shared a net" are different results and only
the second is a finding. Collapsing them is how the test came to assert
`D5 == D10` in the first place.

The accumulation deliberately runs *before* the "port did not change" skip in
`classify_()`. Identical consecutive port words are not noise here — they are
the signature of the thing being looked for. Each GPIO has its own interrupt, so
a single edge on a shared net fires the ISR twice, and the second call stores a
sample with no change in it. Skipping those would throw away the strongest
evidence for the case they indicate.

### The idle-capture blind spot, and the self-provoked sweep

Runs 1–9 shared a defect that produced confident answers rather than obvious
failures: **nothing was ever done to the board during a capture.** Every one of
those runs measured an idle panel, and an idle panel cannot distinguish a
button input from a dead test header. Both look like a pulled-up line at rest.

The scope of the damage is narrower than it sounds, and the distinction is
worth stating precisely because it is the difference between a finding and a
retraction:

| Question | Needs stimulus? | Why |
| --- | --- | --- |
| Is the clock or data among these pins? | **No** | The display bus free-runs at ~10 frames/s regardless of input. Run 9 logged D1 at 5256 edge/s *in the same buffer* where four of the six logged zero. A line on the clock's net cannot be silent while the clock is talking. |
| What *are* these pins? | **Yes** | Four static HIGH lines is exactly what a button input looks like when nobody is pressing the button. |

So the first conclusion stands unexercised; the second was never actually
tested.

The obvious fix — "arm the sweep, then go press buttons" — **cannot work**, and
this is a property of the instrument rather than of the operator. The buffer
fills in a fraction of a second (0.58 s in run 9), arming happens in Home
Assistant, and no hand gets from a phone to the panel in that time. A sweep that
depends on a human to exercise it is a sweep that never gets exercised.

[sweep.yaml](esphome/sweep.yaml) therefore presses the button itself. A
`sweep_provoked` script arms the capture, waits 50 ms, and drives the MODE
switch four times through the same open-drain injection
[live.yaml](esphome/live.yaml) uses. The press is inside the window *by
construction* instead of by luck, and its timing is recorded rather than
recalled.

Three details carry more weight than their size suggests:

- **`script.execute` is followed by `script.wait`.** `press_mode` is
  `mode: single`, so firing it again while it runs is silently *dropped* — a
  bare `repeat` of four executes would collapse into one press and three
  no-ops, and the report would look like a successful exercised run.
- **Four presses, not one.** The panel has four modes, so a full cycle returns
  the pump to where it started. The sweep firmware has no `clocked_tap` and
  cannot read the display, so it does not know the current mode; any count
  other than a full cycle parks the pump somewhere it was not. The cycle does
  transit Always On — the mode with no dry-run protection — for ~420 ms.
- **300 ms between presses, against live.yaml's 400 ms.** Entering Time Mode
  opens a submenu ~800 ms later, after which MODE means "next menu item"
  instead of "next mode". Pressing again inside 300 ms steps past Time Mode
  before its menu can appear.

**The result is read as a difference, not as an absolute.** Every boot still
runs an unprovoked capture after `settle_delay`, which is the control:

| Idle | Provoked | Reading |
| --- | --- | --- |
| static | static | rail, ground, or genuinely unconnected |
| static | **moves** | a button or status line — this is the find |
| moves | moves | free-running; a floating wire does this and it means nothing |

This also forced D1/D2 out of the tap. They contributed essentially all of the
~7800 edge/s that made the window 0.58 s, and the 1.7 s press sequence has to
fit inside it. Their job — anchoring the net-identity test — was finished by
run 9.

### Only the lines that moved are permuted

Before any hypothesis is enumerated, one XOR scan over the buffer works out
which lines carried edges. Everything else is dropped from the search space and
named in the log.

This is what makes tapping a whole header practical. Ten lines is 151 200
ordered HD44780 permutations; the six that move on a real LCD header is 720 —
the same cost as hand-picking the right six, without having to know which six
in advance. The pruning is free of assumptions: a line that never changed
cannot be a strobe, a clock or a TX, and it decodes to a constant, so those
permutations score zero however many of them are tried. Cutting them also
*raises* confidence in the winner, since a smaller search space gives noise
fewer chances to fit.

### The defect that mattered most: a hidden fixed parameter

The clocked family swept clock line, data line, polarity, bit order and
alignment — and silently assumed **8-bit words**. The pump's panel uses 9-bit
words, so the one bus this project actually had was outside the search space
entirely, and no amount of re-capturing would have found it.

What makes this worth writing down is the *shape* of the failure. A hypothesis
that is merely wrong scores badly and invites doubt. This one produced a
confident-looking verdict — `align2 (165 B)` with a real twelve-character
display string in it — that read as a marginal-but-real decode and was treated
as one for days. It took building a second component and a frame-level analysis
to establish that an 8-bit reading of this bus decodes to nothing whatsoever.

The verdict was *more* misleading for containing real text than a low score
would have been. Note also that the mechanism by which that text appeared is
still unexplained: the tidy story (drifting through a lucky phase) was tested
and is false. A wrong fixed parameter does not just cost you the answer; it can
manufacture evidence for itself.

The sweep now tries word lengths `{8, 9}` and prints the length in its verdict,
so the parameter is visible rather than assumed. The payload is taken as the
last eight bits of a word, which makes a leading flag bit fall out for free.

### Four scoring defects the 10-pin run exposed

Tapping a full header broke assumptions that held while only hand-picked lines
were swept. All four were found by the sweep contradicting itself between
consecutive captures of the same wiring, and all four are fixed:

- **Mains vetoed everything, not just the lines it touched.** The detector
  short-circuited the whole report on the theory that a bad ground makes every
  level meaningless. That is true of the coupled pin and of any decode reading
  it — not of the rest of the tap. On a whole-header tap an unused floating
  line is ordinary, and it was suppressing the only real finding in the report.
  The veto now applies only when the winning hypothesis actually reads a
  coupled line.
- **Known-string matching only searched the printable sample.** Those buffers
  are display-sized, ~40 characters. A phrase eighty bytes into a stream was
  invisible. One capture surfaced `Standby Mode` in the first forty bytes and
  the next capture of the same lines put it past the cut and scored the
  interpretation as noise. Whether a hypothesis is right cannot depend on where
  in the window the interesting part happened to fall, so matching now runs
  incrementally over every decoded byte.
- **The clocked sweep picked its byte alignment before checking for hits.**
  Alignment was chosen on printable ratio alone. A shifted alignment smears
  each real character across two output bytes and lands in `0x20-0x7E` often
  enough to beat the alignment that actually spells something. Hits are now
  weighed while the choice is made.
- **A hypothesis was counted as its own rival.** The margin test asks whether
  some *other* explanation fits as well, but entries differing only in clock
  polarity, bit order or alignment are the same wiring decoded slightly
  differently — and on a clock whose data is stable across both edges they
  necessarily score within a point of each other. The runner-up is now the best
  hypothesis reading a *different set of lines*.

Configuration:

| Option | Default | Purpose |
| --- | --- | --- |
| `pins` | required | 2-10 entries, each `pin:` plus optional `label:`. All must be GPIO0-31. |
| `capacity` | `4096` | Samples, 8 bytes each. Allocated once at setup. Max 16384. |
| `settle_delay` | `10s` | Wait before arming, so capture is not competing with WiFi association. |
| `capture_timeout` | `60s` | Capture ends on buffer-full or this, whichever first. |
| `idle_gap` | `200us` | Byte-boundary signal, HD44780 hypothesis only. |
| `columns` | `16` | Panel width for rendered samples. |
| `verdict` | — | Text sensor carrying the summary line. |

A template button re-arms the capture without a reflash. The useful loop is:
press it, then immediately go and work the pump's buttons for the length of the
capture window. **An event-driven header is indistinguishable from a dead one
unless you provoke it.**

### Reading the verdict

| Verdict | Means |
| --- | --- |
| **NO TRAFFIC**, every line static | Almost certainly an ISP/SWD/ICSP programming header. Nothing to sniff; go back to the LCD header. |
| **MAINS COUPLING** | A line is cycling at 20 ms *and* the winning hypothesis reads it. Either that line is floating, or the ground bond did not take — re-check backlight-cathode continuity to LCD pin 4. A coupled line the winner does not read is reported as a note, not a verdict. |
| **NO MATCH**, traffic present | Something is talking, but it is not HD44780, not 8N1, and not a simple clocked stream. |
| **AMBIGUOUS** | Two hypotheses scored close. Capture again with the board being exercised harder. |
| A clear winner | Copy the pin assignment into `hauswasserwerk.yaml`. |

A null result is a real answer, not a failed sweep. "This header carries no
runtime data" is worth having definitively, in one flash, rather than by
inference over weeks.

### Shared decoder: `hd44780_core`

The sweep needs the HD44780 state machine hundreds of times with a reset
between runs, and `hd44780_tap` needs it once, live. Rather than keep two copies
that drift apart, the controller model — DDRAM shadow, the CGRAM-mode
diversion, display shift, and the non-contiguous `0x27 -> 0x40` wrap — lives in
a header-only, ESPHome-free struct in
[esphome/components/hd44780_core/](esphome/components/hd44780_core/). Both
components `AUTO_LOAD` it. It has no configuration of its own; declaring it is
just what gets the directory into the build tree.

`hd44780_tap` keeps only its ISR, ring buffer, nibble framing and sensor
publishing. No behaviour changed in the extraction.

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

**To read this pump's display, flash [live.yaml](esphome/live.yaml)**, not
`hauswasserwerk.yaml`:

```powershell
.\.venv\Scripts\python.exe -m esphome run esphome\live.yaml --device COM3
```

Run it **with logs attached** — that is what plain `run` does. Uploading with
`--no-logs` and reconnecting afterwards is how one sweep run was lost: the log
is not replayed across a reconnect. Free the port first if a previous log
stream is still open, or the flash fails with `Could not open COM3`.

The investigation firmwares build the same way. `sweep.yaml` adds new
sources and moves code between components, so its **first** build should be a
clean one — a stale object file can make a compile appear to succeed while
silently skipping a changed `.cpp`:

```powershell
# Gate: confirm the ground bond took. The 50Hz! markers should be gone.
.\.venv\Scripts\python.exe -m esphome run esphome\probe.yaml --device COM3

# Then identify the header.
.\.venv\Scripts\python.exe -m esphome clean esphome\sweep.yaml
.\.venv\Scripts\python.exe -m esphome run esphome\sweep.yaml --device COM3
```

Run `sweep.yaml` with logs attached — that is what plain `run` does. Uploading
with `--no-logs` and connecting afterwards loses the report: the capture is
over roughly 16 s after reset and the log output is not replayed.

After the first USB flash, updates go over the air.

### Home Assistant

`live.yaml` pulls in [common.yaml](esphome/common.yaml), which already carries
the encrypted native API, so nothing extra is needed to get the pump into Home
Assistant. Flash it, and once the node is on WiFi, HA discovers it by itself:

1. **Settings → Devices & Services**. `Hauswasserwerk` appears under *Discovered*
   as an ESPHome device.
2. It asks for the encryption key. That is `api_encryption_key` in
   `esphome/secrets.yaml` — which is gitignored, and should stay that way.

Two entities carry the display, mirroring the two physical rows:

| Entity | Example |
| --- | --- |
| `sensor.hauswasserwerk_display_line_1` | `Standby Mode` |
| `sensor.hauswasserwerk_display_line_2` | `Not Recommended`, or empty |
| `button.hauswasserwerk_press_mode` | Advances the mode, one position per press |

**`Press MODE` is a real actuator**, not a diagnostic. It advances the pump one
position round `Standby → Automatic → Time → Always On → Standby`, and
`Always On` has no dry-run protection. Watch `Display Line 1` to see where it
landed — one press is not always one visible screen change, and the mode only
moves one step whatever the panel does afterwards. See
[MODE injection](#mode-injection-no-parts-required) and
[Button mapping](#button-mapping).

Both are plain trimmed text, on purpose — see
[What is published is not what is logged](#what-is-published-is-not-what-is-logged)
for why there is no derived "mode" entity and why a blank arrives 1.5 s late.

Six more entities — `binary_sensor.hauswasserwerk_pin_d3` through `pin_d10`,
all marked *diagnostic* — expose the raw state of the
[6-pin header](#why-re-sweep-a-header-that-already-came-back-empty). They exist
to answer what `bus_sweep` structurally cannot: the sweep's buffer fills in a
fraction of a second, so a human reaching for a front-panel button never lands
inside its window. **Press every control on the pump in turn and watch which row
flips.**

Each is filtered with `delayed_on_off: 100ms`, which is doing real work rather
than just debouncing:

| Line behaviour | What you see |
| --- | --- |
| Button press, hundreds of ms | fully visible |
| Slow status line | visible |
| Fast bus line, never holds 100 ms | **publishes nothing** — frozen at its boot state |

So a row that never moves is ambiguous *by design* — static line or fast bus —
and the sweep is what tells those apart. That is the right trade: without the
filter, a line sharing a net with the 6 kHz display clock would write thousands
of states a second into HA's recorder.

They are also configured `use_interrupt: false`, against the platform default.
An any-edge ISR on a line that turned out to carry the clock would land 6650
interrupts a second on the same core as `clocked_tap`'s own ISR, and the tap is
sensitive enough that a held-open web UI alone dropped it from 90 frames/s to
16. Polling loses nothing here, because the filter has already discarded
everything faster than 100 ms.

The node's hostname stays `hauswasserwerk-live` while the HA-visible name is
`Hauswasserwerk`, so this can share a network with anything flashed from the
superseded `hauswasserwerk.yaml` without two nodes fighting over one mDNS name.

**Do not leave the web UI open while capturing.** Holding
`http://hauswasserwerk-live.local/` open dropped the frame rate from ~90/s to
16/s and started losing edges in the ISR — one 160 MHz core cannot serve a
3.8 kedge/s interrupt and stream HTTP at the same time. The native API is a
binary push and does not do this, so Home Assistant is a fine consumer; the web
page is a debugging tool only. If `dropped` ever goes non-zero during protocol
work, check for an open browser tab before suspecting the wiring.

## Roadmap

1. **Finish the LCD header.** ✅ **Done.** The protocol is decoded, the display
   renders continuously, and the MODE button has been mapped through a full
   four-position cycle — see [`clocked_tap`](#the-clocked_tap-component) and
   [Button mapping](#button-mapping). Remaining on this line: pull the two
   power rails off the XIAO, meter D0-D9 to record the physical pin map, and
   fit the 74LVC245 buffer — logic level is still unsettled. The 6-pin header
   is a closed question: [swept, no runtime
   traffic](#the-6-pin-header-swept).
2. **External power monitoring.** A Shelly PM Mini Gen3 on the motor feed —
   metering-only, no relay contacts taking a 1300 W induction motor's inrush on
   every cycle. Fully external, zero risk, and gives the ground truth every
   later stage checks against.
3. **Button injection + auto-rearm.** MODE injection is ✅ **done** — a
   `Press MODE` button in Home Assistant, driving XIAO D7/D8 open-drain with no
   extra parts, because one terminal of the switch is grounded. See
   [MODE injection](#mode-injection-no-parts-required). Still outstanding: the
   other two buttons, and *auto*-rearm, which is the part that needs the
   cold-boot guard and the `CheckWater` interlock from
   [Safety interlocks](#safety-interlocks). Nothing presses by itself yet, and
   that is deliberate — a human in the HA UI is the only trigger.
4. **LCD sniffer.** ✅ **Working, and in Home Assistant.**
   [`clocked_tap`](#the-clocked_tap-component)
   renders both display rows live off two wires, has been watched through a
   full pass of the MODE button, and publishes both rows as trimmed text
   entities over the encrypted native API — see
   [Home Assistant](#home-assistant). The earlier
   [`hd44780_tap`](#the-hd44780_tap-component) targets a bus this panel does
   not have and has never seen real traffic.
5. **Bus probe.** ✅ Written, flashed, and it has already earned its keep: it
   proved the tap had no valid ground reference before a single byte was
   misdecoded. See [The 6-pin header, probed](#the-6-pin-header-probed).
6. **Bus sweep.** ✅ Written and flashed. It found the right two wires, the
   right clock edge and the right bit order — and then reported a word length
   it had never actually tried. Enough to point at the bus, not enough to read
   it. See [The 10-pin LCD header, swept](#the-10-pin-lcd-header-swept) and
   [the hidden-parameter defect](#the-defect-that-mattered-most-a-hidden-fixed-parameter),
   since fixed.
7. *Optional:* inline 0-10 bar pressure transducer and a YF-B10 brass Hall flow
   sensor, read via ADS1115. A continuous pressure curve is more diagnostic
   than any discrete display state — it shows bladder pre-charge degradation,
   slow leaks, and dry-run onset before the MCU latches.

## Status

**The display is decoded and rendering live.** The pump's panel is a two-wire
clocked serial bus — D1 clock, D2 data, MSB-first, **9-bit words** — and
[`clocked_tap`](#the-clocked_tap-component) publishes both rows continuously at
10 frames/s with zero dropped samples and zero undecoded frames over minutes of
capture. Pressing MODE on the front panel produces a decoded screen change in
the log within ~100 ms, and the full four-position mode cycle is
[mapped](#button-mapping).

That closes the question the whole investigation was blocked on, and it
corrects the one this README got wrong for several revisions: **the panel is
not an HD44780 parallel bus.** Six logic-level readings were inferred as
`RS, E, D4-D7` from static DC voltages; in fact only two of the ten lines ever
move. The sweep found those two, but reported a decode it could not actually
produce, because its clocked hypothesis silently assumed 8-bit words — see
[the hidden-parameter defect](#the-defect-that-mattered-most-a-hidden-fixed-parameter).

Still outstanding, none of it blocking:

1. **Two wires must come off the XIAO.** The +4.87 V supply and the negative
   contrast rail are still in the tap, clamping through the protection diodes.
   They are among the static lines — HIGHs {D0, D4, D6}, LOWs {D3, D5, D9}.
   This is the only item with a safety dimension.
2. **The physical pin map is unrecorded.** Which LCD header position each XIAO
   pin sits on was never written down. Needed to reproduce the wiring, not to
   read the bus. A meter job.
3. **Logic level is unsettled** — unresolved item 2, fit the 74LVC245.
4. **Two frame word groups are unidentified**, the leading `F8 00 00` and the
   inter-line `3E 40 60 1F 10`. Skipped, not decoded. Likely cursor/addressing
   commands; not needed for text.

`hd44780_tap` **has never seen a real bus** and, on this pump, never will —
it targets a bus this panel does not have. It stays as correct code for panels
that do, and for the `hd44780_core` decoder that `bus_sweep` shares.

The **6-pin header** wired to D0-D5 was swept earlier and carries **no runtime
digital traffic**: four lines strapped high, two strapped low, zero edges in
60 s, confirmed by an independent polled cross-check. See
[The 6-pin header, swept](#the-6-pin-header-swept). That points at a
programming or test header. It is a closed question in practice now that the
LCD header is producing text, though it was never exercised during a capture.

Superseded: an earlier UART probe firmware, written before the bus measurements
existed. There is no serial line on this board, so it could never have produced
data. Its connectivity, antenna, logging and diagnostics sections were carried
over into the current firmware.
