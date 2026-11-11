"""Recover the display bitstream from the DATA line alone, with no clock wire.

Reads an edge_dump log, estimates the bit period by quantisation error,
reconstructs the NRZ bit string, then hunts the known 7C 40 line marker at
every bit offset and renders the 16 characters that follow it.

The known-answer check is the whole point: the 2026-09 protocol work fixed the
word length (9 bits), the marker (7C 40), the bit order (MSB first) and the
expected strings ("Standby Mode", "Automatic Mode", ...). If those fall out of
a stream reconstructed without a clock, the recovery worked. If they do not,
it failed, and the clock wire is required.

Usage:  python tools/recover.py <logfile>
"""

import re
import sys
from collections import Counter

KNOWN = [
    "Standby Mode", "Automatic Mode", "Time Mode", "Always On",
    "Not Recommended", "NOT SET", "PUMPOFF", "CheckWater", "ValveClosed",
    "Time Now", "Save", "Exit", "Mode", "On",
]

FRAME_RE = re.compile(r"=== FRAME (\d+): (\d+) edges, span (\d+) us, first level (\d+) ===")
PAIR_RE = re.compile(r"\b([01]):(\d+)\b")


def parse_frames(text):
    """Pull out (index, first_level, [(level, duration), ...]) per frame."""
    frames = []
    cur = None
    for line in text.splitlines():
        line = line.rstrip("\r")
        m = FRAME_RE.search(line)
        if m:
            cur = {"idx": int(m.group(1)), "first": int(m.group(4)), "runs": []}
            continue
        if cur is None:
            continue
        if "=== END FRAME" in line:
            frames.append(cur)
            cur = None
            continue
        # Only accept lines that are purely run-length cells, so stray log
        # output can never be parsed as timing data.
        body = line.split("]: ")[-1].strip()
        if body and all(PAIR_RE.fullmatch(tok) for tok in body.split()):
            for tok in body.split():
                lvl, dur = tok.split(":")
                cur["runs"].append((int(lvl), int(dur)))
    return frames


def quant_error(runs, period):
    """Mean |d/T - round(d/T)| over the runs. 0 = perfect integer multiples."""
    err = 0.0
    n = 0
    for _lvl, d in runs:
        q = d / period
        r = round(q)
        if r < 1:
            r = 1
        err += abs(q - r)
        n += 1
    return err / max(n, 1)


def best_period(runs, lo=5.0, hi=60.0, step=0.01):
    best, best_err = None, 1e9
    t = lo
    while t <= hi:
        e = quant_error(runs, t)
        if e < best_err:
            best_err, best = e, t
        t += step
    return best, best_err


def to_bits(runs, period):
    bits = []
    for lvl, d in runs:
        n = max(1, round(d / period))
        bits.extend([lvl] * n)
    return bits


def decode(bits, word_bits, align, msb_first, marker=(0x7C, 0x40), columns=16):
    """Read payload bytes as the LAST 8 bits of each word, from `align`."""
    out = []
    i = align
    while i + word_bits <= len(bits):
        w = bits[i + word_bits - 8: i + word_bits]
        if msb_first:
            b = 0
            for x in w:
                b = (b << 1) | x
        else:
            b = 0
            for k, x in enumerate(w):
                b |= x << k
        out.append(b)
        i += word_bits
    return out


def render(by, marker=(0x7C, 0x40), columns=16):
    lines = []
    for i in range(len(by) - 1):
        if by[i] == marker[0] and by[i + 1] == marker[1]:
            chars = by[i + 2: i + 2 + columns]
            s = "".join(chr(c) if 32 <= c < 127 else "?" for c in chars)
            lines.append((i, s))
    return lines


def score(s):
    return sum(len(k) for k in KNOWN if k.lower() in s.lower())


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/recover_log.txt"
    with open(path, "r", errors="replace") as fh:
        frames = parse_frames(fh.read())

    print(f"parsed {len(frames)} frames")
    if not frames:
        return

    allruns = [r for f in frames for r in f["runs"]]
    print(f"total runs: {len(allruns)}")

    durs = sorted(d for _l, d in allruns)
    print(f"duration range: {durs[0]} .. {durs[-1]} us")
    print("most common durations:",
          Counter(d for _l, d in allruns).most_common(12))

    period, err = best_period(allruns)
    print(f"\nbest bit period: {period:.2f} us  (mean quantisation error {err:.4f})")

    # Independent cross-check: span / bits, against the documented 382-bit frame.
    f0 = frames[0]
    span = sum(d for _l, d in f0["runs"])
    print(f"frame 0 span {span} us -> {span/period:.1f} bits at that period")
    print(f"  (382-bit frame would imply T = {span/382:.2f} us;"
          f" 191-bit frame T = {span/191:.2f} us)")

    best = None
    for f in frames[:4]:
        bits = to_bits(f["runs"], period)
        for wb in (9, 8, 10):
            for msb in (True, False):
                for align in range(wb):
                    by = decode(bits, wb, align, msb)
                    for pos, s in render(by):
                        sc = score(s)
                        if sc and (best is None or sc > best[0]):
                            best = (sc, f["idx"], wb, align, msb, pos, s)

    print("\n--- marker-driven decode over 4 frames, "
          "word lengths {8,9,10} x both bit orders x every alignment ---")
    if best:
        sc, idx, wb, align, msb, pos, s = best
        print(f"BEST HIT score={sc} frame={idx} word_bits={wb} align={align} "
              f"msb_first={msb} at byte {pos}:")
        print(f"    [{s}]")
    else:
        print("NO HIT. The 7C 40 marker never appeared followed by known text.")
        # Show what the stream looks like anyway, so the failure is inspectable.
        bits = to_bits(frames[0]["runs"], period)
        print(f"frame 0 reconstructed to {len(bits)} bits")
        for wb in (9, 8):
            by = decode(bits, wb, 0, True)
            hexs = " ".join(f"{b:02X}" for b in by[:40])
            txt = "".join(chr(b) if 32 <= b < 127 else "." for b in by[:40])
            print(f"  {wb}-bit align0 MSB: {hexs}")
            print(f"  {'':16} as text: {txt}")


if __name__ == "__main__":
    main()
