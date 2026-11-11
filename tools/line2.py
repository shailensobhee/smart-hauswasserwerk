"""Find the second display line, which does not share line 1's bit phase.

Line 1 decodes cleanly at bit offset 0, but the words after it are not ASCII.
The 2026-09 protocol note says line 2 starts two bits out of phase with line 1,
which is exactly what would turn readable text into the observed garbage: a
constant bit slip shifts every subsequent word boundary.

So rather than assume the offset, scan every bit offset in the frame for the
7C 40 marker and render whatever follows. The flag bit (the 9th, leading bit of
each word) is printed alongside, because on this protocol it is what separates
a command word from a character word - and a correct alignment should show the
16 characters all carrying the same flag.

Usage:  python tools/line2.py <logfile>
"""

import sys

from recover import parse_frames, to_bits

PERIOD = 25.50
WORD_BITS = 9


def words_from(bits, start):
    """(flag, byte) pairs: leading bit is the flag, trailing 8 are the payload."""
    out = []
    i = start
    while i + WORD_BITS <= len(bits):
        w = bits[i:i + WORD_BITS]
        b = 0
        for x in w[1:]:
            b = (b << 1) | x
        out.append((w[0], b))
        i += WORD_BITS
    return out


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/recover_log.txt"
    with open(path, "r", errors="replace") as fh:
        frames = parse_frames(fh.read())

    bits = to_bits(frames[0]["runs"], PERIOD)
    print(f"frame 0: {len(bits)} bits "
          f"(documented full frame is 382; two 191-bit halves)\n")

    print("--- every bit offset that puts a 7C 40 marker on a word boundary ---")
    hits = []
    for off in range(len(bits) - 18 * WORD_BITS):
        ws = words_from(bits, off)
        if len(ws) < 18:
            continue
        if ws[0][1] == 0x7C and ws[1][1] == 0x40:
            chars = ws[2:18]
            s = "".join(chr(b) if 32 <= b < 127 else "?" for _f, b in chars)
            flags = "".join(str(f) for _b, f in [(b, f) for f, b in chars])
            hits.append((off, s, flags))

    for off, s, flags in hits:
        print(f"  bit {off:3d}:  [{s}]   char flags: {flags}")

    if len(hits) >= 2:
        d = hits[1][0] - hits[0][0]
        print(f"\nTwo markers, {d} bits apart.")
        print(f"  191-bit half frame + 2-bit phase slip would predict 191.")

    print("\n--- both halves as (flag)byte, frame 0 ---")
    for off, label in [(h[0], f"LINE at bit {h[0]}") for h in hits]:
        ws = words_from(bits, off)[:18]
        print(f"  {label}:")
        print("    " + " ".join(f"{f}{b:02X}" for f, b in ws))


if __name__ == "__main__":
    main()
