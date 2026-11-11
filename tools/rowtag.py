"""Find out how the panel says WHICH ROW a half-frame is for.

Background: clocked_tap slots lines by the ORDER the two `7C 40` markers appear
in the frame - first marker = line 1, second = line 2. On 2026-11-11 the user
read the glass during a Time edit and found that wrong: the panel shows

    line 1   Time Now   00:00
    line 2   Next    +     -

but Home Assistant had them swapped. The firmware's own log agrees, so the
panel really does transmit ROW 2 FIRST on that screen, and order is not a
reliable slotting rule.

This script replays the raw frame dumps (the `382 bits | 7C 40 ...` lines the
component logs at level DEBUG) through the exact same bit/word arithmetic the
C++ uses, and prints the words after each marker, so a row discriminator can be
found in the data rather than guessed at.

Usage:  python tools/rowtag.py [capture.txt ...]
"""

import re
import sys

WORD_BITS = 9
MARKER_A = 0x7C
MARKER_B = 0x40
COLUMNS = 16

# The three screens below are lifted verbatim from captures/live_log.txt around
# 04:15, which is the only capture that contains the Time edit screen. The
# `glass` field is what the panel was ACTUALLY showing, as decoded by the
# firmware at the time (and, for the edit screen, as confirmed by the user's
# own eyes on 2026-11-11).
SAMPLES = [
    (
        "Time Mode (resting)",
        ["   Time Mode    ", "                "],
        "7C 40 20 07 C2 00 80 40 20 2A 1A 4D A6 51 01 34 DE 64 32 88 04 02 01 00 "
        "F8 80 60 0F 84 01 00 80 40 20 10 08 04 02 01 00 80 40 20 10 08 04 02 00",
    ),
    (
        "Time Now (resting, soft keys)",
        ["Time Now   00:00", "Save  Set   Exit"],
        "7C 40 20 07 C2 01 50 D2 6D 32 88 09 C6 F3 B8 80 40 20 18 0C 07 43 01 80 "
        "F8 80 60 0F 84 02 99 84 EC 65 10 08 0A 66 53 A0 80 40 20 22 9E 0D 27 40",
    ),
    (
        "Time Now (EDITING - rows transmitted in the other order)",
        ["Time Now   00:00", "Next    +     - "],
        "7C 40 30 07 C2 01 38 CA 78 3A 08 04 02 01 00 AC 40 20 10 08 04 02 D1 00 "
        "F8 80 40 0F 84 02 A1 A4 DA 65 10 13 8D E7 71 00 80 40 30 18 0E 86 03 00",
    ),
]


def bits_of(hexdump):
    """Unpack the logged hex into a list of bits, MSB first within each byte."""
    out = []
    for token in hexdump.split():
        byte = int(token, 16)
        for k in range(7, -1, -1):
            out.append((byte >> k) & 1)
    return out


def word_data(bits, start):
    """The 8 payload bits of the 9-bit word at `start` - i.e. its LAST 8 bits.

    Identical to ClockedTap::word_data_() for word_bits=9, msb_first.
    """
    b = start + WORD_BITS - 8
    v = 0
    for k in range(8):
        if b + k >= len(bits):
            return None
        v = (v << 1) | bits[b + k]
    return v


def markers(bits):
    """Every bit offset carrying a `7C 40` marker pair, scanned bit by bit."""
    found = []
    i = 0
    while i + 2 * WORD_BITS <= len(bits):
        if word_data(bits, i) == MARKER_A and word_data(bits, i + WORD_BITS) == MARKER_B:
            found.append(i)
            i += 2 * WORD_BITS
        else:
            i += 1
    return found


def printable(v):
    return chr(v) if v is not None and 0x20 <= v < 0x7F else "."


def main():
    for title, glass, dump in SAMPLES:
        bits = bits_of(dump)
        offs = markers(bits)
        print(f"\n=== {title}")
        print(f"    glass row 1: [{glass[0]}]")
        print(f"    glass row 2: [{glass[1]}]")
        print(f"    {len(bits)} bits, markers at {offs}")

        for n, off in enumerate(offs):
            # Words after the marker pair, in transmission order.
            start = off + 2 * WORD_BITS
            words = []
            j = start
            while len(words) < COLUMNS + 2 and j + WORD_BITS <= len(bits):
                words.append(word_data(bits, j))
                j += WORD_BITS
            text = "".join(printable(w) for w in words[:COLUMNS])
            # Which glass row does this half-frame actually correspond to?
            row = "?"
            for r, g in enumerate(glass):
                if text.rstrip() == g.rstrip():
                    row = str(r + 1)
            hexs = " ".join("--" if w is None else f"{w:02X}" for w in words)
            print(f"      marker #{n + 1} @bit {off:4d} -> [{text}]  (glass row {row})")
            print(f"        words: {hexs}")

    # The discriminator, stated as a check rather than a claim.
    print("\n=== Is the word BEFORE the marker a row tag?")
    for title, glass, dump in SAMPLES:
        bits = bits_of(dump)
        offs = markers(bits)
        print(f"  {title}")
        for n, off in enumerate(offs):
            pre = word_data(bits, off - WORD_BITS) if off >= WORD_BITS else None
            start = off + 2 * WORD_BITS
            text = "".join(
                printable(word_data(bits, start + k * WORD_BITS)) for k in range(COLUMNS)
            )
            row = next((str(r + 1) for r, g in enumerate(glass) if text.rstrip() == g.rstrip()), "?")
            pres = "--" if pre is None else f"{pre:02X}"
            print(f"    half #{n + 1}: preceding word {pres}  -> glass row {row}")


ROW_TAG = {0x00: 1, 0x80: 2}


def row_tag(bits, off):
    """The row this half-frame targets, from the command word before the marker.

    Returns 1, 2 or None if the word is not a recognised tag.
    """
    if off < WORD_BITS:
        return None
    return ROW_TAG.get(word_data(bits, off - WORD_BITS))


def validate(path):
    """Replay every logged frame and check the tag against the decoded screen.

    The firmware logs a screen line `[row a][row b]` immediately followed by the
    raw `N bits | ...` dump of the frame it came from, so the two can be paired
    and the tag checked against what the decoder produced in transmission order.
    """
    screen_re = re.compile(r"clocked_tap:\d+\]: \[(.*?)\]\[(.*?)\]\s*$")
    dump_re = re.compile(r"clocked_tap:\d+\]:\s+(\d+) bits \| ([0-9A-F ]+?)\s*$")

    pending = None
    stats = {"frames": 0, "tagged": 0, "untagged": 0, "swapped": 0, "same_tag": 0}
    swapped_screens = {}
    untagged_examples = []

    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = screen_re.search(line)
            if m:
                pending = (m.group(1), m.group(2))
                continue
            m = dump_re.search(line)
            if not m or pending is None:
                continue

            bits = bits_of(m.group(2))
            offs = markers(bits)
            if len(offs) != 2:
                pending = None
                continue

            stats["frames"] += 1
            tags = [row_tag(bits, o) for o in offs]
            if None in tags:
                stats["untagged"] += 1
                if len(untagged_examples) < 5:
                    raw = [
                        None if o < WORD_BITS else word_data(bits, o - WORD_BITS) for o in offs
                    ]
                    untagged_examples.append((pending, raw))
            elif tags[0] == tags[1]:
                stats["same_tag"] += 1
            else:
                stats["tagged"] += 1
                if tags[0] == 2:  # row 2 transmitted first
                    stats["swapped"] += 1
                    key = f"[{pending[0]}][{pending[1]}]"
                    swapped_screens[key] = swapped_screens.get(key, 0) + 1
            pending = None

    print(f"\n=== Validation against {path}")
    print(f"    two-marker frames paired with a screen : {stats['frames']}")
    print(f"    both halves carry a valid, DISTINCT tag: {stats['tagged']}")
    print(f"    tag word unrecognised                  : {stats['untagged']}")
    print(f"    both halves claim the SAME row         : {stats['same_tag']}")
    print(f"    row 2 transmitted FIRST (order wrong)  : {stats['swapped']}")
    if swapped_screens:
        print("\n    screens where the order is reversed, as decoded in transmission order:")
        for k, v in sorted(swapped_screens.items(), key=lambda kv: -kv[1]):
            print(f"      {v:5d} x {k}")
    if untagged_examples:
        print("\n    unrecognised tag examples:")
        for screen, raw in untagged_examples:
            vals = " ".join("--" if r is None else f"{r:02X}" for r in raw)
            print(f"      {vals}  [{screen[0]}][{screen[1]}]")


if __name__ == "__main__":
    main()
    for arg in sys.argv[1:] or ["captures/live_log.txt"]:
        validate(arg)
