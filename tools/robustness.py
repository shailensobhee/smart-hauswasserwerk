"""How much margin does the clock-free decode actually have?

The recovery turns a run of duration d into round(d/T) bits. A wrong T does not
fail gracefully: the error accumulates along a run, and the first run long
enough to push the accumulated error past half a bit inserts or drops a bit,
which shifts every word after it and destroys the rest of the frame.

So the question that decides whether one wire is enough is not "does it work"
(it does) but "how wrong can T be before it stops working" - and that depends
on the LONGEST CONSTANT RUN the display ever emits, which depends on screen
content we have not captured yet.

Usage:  python tools/robustness.py <logfile>
"""

import sys

from recover import decode, parse_frames, render, to_bits

WORD_BITS, ALIGN, MSB = 9, 0, True
EXPECTED = ["Time Mode", "NOT SET"]


def decodes_ok(frames, period):
    """True if every frame still yields both expected strings at this period."""
    for f in frames:
        bits = to_bits(f["runs"], period)
        found = []
        # Scan all bit offsets, as line 2 does not share line 1's phase.
        for off in range(len(bits) - 18 * WORD_BITS):
            by = decode(bits[off:], WORD_BITS, ALIGN, MSB)
            for _pos, s in render(by[:2] and by):
                found.append(s)
            if len(found) >= 2:
                break
        joined = " ".join(found)
        if not all(e in joined for e in EXPECTED):
            return False
    return True


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else \
        "captures/2026-11-10-lcd-pin2-idle.txt"
    with open(path, "r", errors="replace") as fh:
        frames = parse_frames(fh.read())

    runs = [d for f in frames for _l, d in f["runs"]]
    longest = max(runs)
    print(f"{len(frames)} frames, {len(runs)} runs")
    print(f"longest constant run observed: {longest} us = "
          f"{longest/25.5:.1f} bit periods\n")

    print("--- sweep T, does the full decode survive? ---")
    lo = hi = None
    t = 23.0
    while t <= 28.0:
        ok = decodes_ok(frames, t)
        if ok:
            lo = t if lo is None else lo
            hi = t
        t = round(t + 0.05, 2)

    if lo is None:
        print("  nothing decoded - check the capture")
        return
    centre = (lo + hi) / 2
    print(f"  decodes correctly for T in [{lo:.2f}, {hi:.2f}] us")
    print(f"  that is +/- {(hi-lo)/2/centre*100:.1f}% around {centre:.2f} us")

    print("\n--- what that margin buys ---")
    # A run of N bits slips when accumulated error exceeds half a bit:
    #   N * (dT/T) > 0.5  ->  N_max = 0.5 / (dT/T)
    frac = (hi - lo) / 2 / centre
    print(f"  tolerable fractional error in T: {frac*100:.1f}%")
    print(f"  a run slips a bit once N * {frac:.4f} > 0.5,")
    print(f"  i.e. at a constant run longer than ~{0.5/frac:.0f} bit periods")
    print(f"       (~{0.5/frac*centre/1000:.2f} ms)")
    print(f"  longest run actually seen: {longest/centre:.1f} bit periods")


if __name__ == "__main__":
    main()
