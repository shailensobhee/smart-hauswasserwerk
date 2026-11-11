"""Press a button on the pump node over ESPHome's native API.

Why this exists rather than curl: this node's web_server build serves the
SPA and /events but returns 404 for the REST endpoints, so there is no HTTP
way to invoke a button. The native API is the supported path anyway - it is
what Home Assistant uses, so pressing through it exercises the same code path
HA will.

    python tools/press.py <host> mode|set|save [count]
    python tools/press.py <host> mode:1,mode:3,set:3,save:3

The second form is a timed SEQUENCE - `button:seconds_to_watch_after`. It
exists because the buttons are soft keys and the panel moves on its own:
entering Time Mode opens a menu ~2 s later by itself and times that menu out
~20 s after that. Pressing SET "in the Time menu" therefore means pressing it
inside a ~20 s window, which separate invocations of this script cannot hit -
each one spends seconds reconnecting. One connection, scripted gaps.

Reads api_encryption_key from esphome/secrets.yaml. Prints the two display
lines as they change, so the effect of each press is visible without reading
the serial log.

SAFETY: this presses a real button on a real pump. MODE walks
Standby -> Automatic -> Time Mode -> Always On -> Standby, and Always On has
no dry-run protection. There is no interlock here, by design.
"""

import asyncio
import re
import sys
from pathlib import Path

from aioesphomeapi import APIClient

ROOT = Path(__file__).resolve().parent.parent
BUTTONS = {"mode": "press_mode", "set": "press_set", "save": "press_save"}


def encryption_key() -> str:
    text = (ROOT / "esphome" / "secrets.yaml").read_text(encoding="utf-8")
    m = re.search(r"^api_encryption_key:\s*\"?([A-Za-z0-9+/=]+)\"?", text, re.M)
    if not m:
        raise SystemExit("api_encryption_key not found in esphome/secrets.yaml")
    return m.group(1)


def parse_steps(argv: list) -> list:
    """-> [(button, watch_seconds), ...] from either argument form."""
    spec = argv[2]
    if "," in spec or ":" in spec:
        steps = []
        for part in spec.split(","):
            name, _, secs = part.partition(":")
            if name not in BUTTONS:
                raise SystemExit(f"unknown button {name!r}")
            steps.append((name, float(secs) if secs else 6.0))
        return steps
    if spec not in BUTTONS:
        raise SystemExit(f"unknown button {spec!r}")
    count = int(argv[3]) if len(argv) > 3 else 1
    return [(spec, 6.0)] * count


async def main(host: str, steps: list) -> None:
    client = APIClient(host, 6053, None, noise_psk=encryption_key())
    await client.connect(login=True)

    entities, _services = await client.list_entities_services()
    keys = {}
    lines = {}
    for group in entities:
        for e in group if isinstance(group, list) else [group]:
            oid = getattr(e, "object_id", None)
            for name, obj in BUTTONS.items():
                if oid == obj:
                    keys[name] = e.key
            if oid in ("display_line_1", "display_line_2"):
                lines[e.key] = oid

    missing = {n for n, _ in steps} - set(keys)
    if missing:
        raise SystemExit(f"no button(s) {sorted(missing)} on {host}")

    # Subscribe first, so the screen that follows a press is never missed.
    seen = {}

    def on_state(state):
        if state.key in lines:
            seen[lines[state.key]] = state.state

    client.subscribe_states(on_state)
    await asyncio.sleep(2.0)

    last = None

    def show(tag=""):
        nonlocal last
        cur = (seen.get("display_line_1", ""), seen.get("display_line_2", ""))
        if cur != last:
            print(f"    [{cur[0]:<16}] / [{cur[1]:<16}] {tag}")
            last = cur

    show("(start)")
    for name, watch in steps:
        client.button_command(keys[name])
        print(f"  >>> {name.upper()}")
        # Poll rather than sleeping through: the panel moves on its own and a
        # screen that appears and vanishes inside the gap is the interesting
        # one. Only changes print, so this stays readable.
        for _ in range(int(watch * 4)):
            await asyncio.sleep(0.25)
            show()

    await client.disconnect()


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    asyncio.run(main(sys.argv[1], parse_steps(sys.argv)))
