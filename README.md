# smart-hauswasserwerk

Monitoring for a domestic water pressure system (Hauswasserwerk), built on a
Seeed Studio XIAO ESP32C6 running ESPHome and reporting into Home Assistant.

The ESP32 passively taps the serial line of the existing pump controller and
decodes it - it does not drive the pump or write to the tapped bus.

## Hardware

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

## Identifying the tap point

### Safety check first

The pump controller is mains powered. If it uses a non-isolated supply, its
logic ground can sit at mains potential - bonding that to an ESP32 plugged
into a PC's USB port is dangerous and destroys hardware.

With the controller powered and nothing connected to it, measure **AC volts**
between its logic ground and mains earth:

- Near 0 V - isolated, safe to continue.
- Tens of volts or more - **not isolated**. Do not share ground with USB. The
  tap must go through an optocoupler, and the ESP32 must run from an isolated
  supply.

### Then identify the signal

Meter on **DC volts**, black probe on the controller's logic ground, red probe
on the candidate pin, read while the line is idle:

| Idle reading | Interface | Needs |
| --- | --- | --- |
| Steady +3.3 V | 3.3 V TTL UART | Direct connection |
| Steady +5 V | 5 V TTL UART | 10k/20k divider |
| Negative, -5 to -12 V | RS-232 | MAX3232 |
| Two lines ~1-3 V, small difference | RS-485 | MAX485, likely Modbus RTU |

UART idles high (positive); RS-232 idles negative. That sign is the reliable
discriminator. A reading that will not settle indicates an active TX line -
that is the one to tap.

## Wiring the tap

The tap is **listen-only**: only the controller's TX line is connected, to
`D7` / `GPIO17`. Nothing is wired to the ESP32's TX, so it is electrically
incapable of disturbing the tapped bus.

**ESP32-C6 GPIOs are 3.3 V and are not 5 V tolerant.** Match the signal before
connecting anything:

| Tapped signal | What is needed |
| --- | --- |
| 3.3 V TTL UART | Direct connection, plus common ground |
| 5 V TTL UART | Resistor divider into GPIO17 (e.g. 10k series / 20k to GND) |
| RS-232 (+/-12 V) | MAX3232 transceiver - direct connection destroys the pin |
| RS-485 (Modbus) | MAX485/MAX3485 transceiver across A/B |

In all cases the two boards must share a ground reference.

## Setup

Toolchain lives in a project-local virtualenv, so nothing is installed
globally beyond Python itself.

```bash
python -m venv .venv
./.venv/Scripts/python.exe -m pip install esphome
```

Copy the secrets template and fill in your WiFi:

```bash
cp esphome/secrets.yaml.example esphome/secrets.yaml
```

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
directory`. In an **elevated** PowerShell, then reboot:

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

# Watch the tapped stream (over the network, once WiFi is up)
.\.venv\Scripts\python.exe -m esphome logs esphome\hauswasserwerk.yaml
```

After the first USB flash, updates go over the air - no cable needed.

## Status

Stage 1: probe firmware that dumps every received UART frame as hex and ASCII,
to reverse engineer the controller's protocol. The baud rate is a guess
(`tap_baud_rate` in the YAML) until real traffic confirms it.

Stage 2 (once the protocol is known): parse the frames into proper Home
Assistant entities - pressure, pump state, run hours, fault codes.
