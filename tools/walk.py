"""Decode a MODE walk: every screen the panel showed, in order, with the
presses that caused them.

recover.py showed the decoder reproduces on one static screen. That is a weaker
claim than it looks - a decoder that always emits the single string it was
tuned on would pass that test. This one cannot be passed by accident: the walk
visits four modes whose screens were documented in 2026-09, and the decoder has
to produce the right text at the right moment, four different times, from
captures it has never seen.

The log interleaves two things, and both matter:
  >>> markers  - what the firmware DID, and when
  FRAME blocks - what the panel SHOWED, and when

So this walks the log in order rather than collecting frames first, and prints
a timeline of screen CHANGES. Printing all 400 frames would bury the result;
the interesting events are the transitions.

Usage:  python tools/walk.py <logfile>
"""

import re
import sys

from recover import best_period, parse_frames, to_bits

WORD_BITS = 9
FRAME_RE = re.compile(r"=== FRAME (\d+): (\d+) edges, span (\d+) us")
PAIR_RE = re.compile(r"\b([01]):(\d+)\b")
TS_RE = re.compile(r"\[(\d\d:\d\d:\d\d)\]")
MARK_RE = re.compile(r">>> (.*)")

EXPECTED = ["Time Mode", "NOT SET", "Always On", "Standby Mode",
            "Automatic Mode", "Time Now", "Save", "Exit"]

# The four mode names, in the order the 2026-09 button mapping says MODE walks
# them. Written down before the run; the decoder has to reproduce it.
MODE_NAMES = ["Standby Mode", "Automatic Mode", "Time Mode", "Always On"]
PREDICTED = ["Standby Mode", "Automatic Mode", "Time Mode",
             "Always On", "Standby Mode"]


def words_from(bits, start):
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


# A frame carries 382 bits as two 191-bit halves, and the two line markers in
# the reference capture sat at bit 27 and bit 218. So a marker found below this
# threshold belongs to line 1, above it to line 2. The panel sometimes redraws
# only ONE line, emitting a short 191-bit frame, and without this split such a
# frame is ambiguous about which line it just updated.
LINE2_BIT_THRESHOLD = 120


def read_slots(runs, period):
    """{slot: text} for the lines this frame actually redrew. Slot is 0 or 1.

    A frame does not necessarily carry both lines - the protocol truncates to a
    191-bit half frame when only one line is redrawn. Returning a dict rather
    than a list keeps "line 2 was not sent" distinct from "line 2 is blank",
    which a bare list silently conflates.
    """
    bits = to_bits(runs, period)
    found = []
    off = 0
    while off < len(bits) - 18 * WORD_BITS:
        ws = words_from(bits, off)
        if len(ws) >= 18 and ws[0][1] == 0x7C and ws[1][1] == 0x40:
            s = "".join(chr(b) if 32 <= b < 127 else "?"
                        for _f, b in ws[2:18])
            found.append((off, s))
            off += 18 * WORD_BITS
        else:
            off += 1

    # Slot by ORDER when both lines are present, and only fall back to the
    # absolute offset when there is a single marker to place.
    #
    # Absolute offset alone is not safe. In capture 1 one frame carries its
    # markers at bit 1777 and 1968 - the 1748-bit idle gap - so an absolute
    # threshold calls BOTH of them line 2 and silently drops line 1. The two
    # markers are 191 bits apart wherever the frame starts, so their order
    # within the frame is the reliable discriminator and their absolute
    # position is not.
    if len(found) >= 2:
        return {0: found[0][1], 1: found[1][1]}
    if len(found) == 1:
        off, s = found[0]
        return {0 if off < LINE2_BIT_THRESHOLD else 1: s}
    return {}


class Display:
    """The two-line panel as a stateful thing, which is what it physically is.

    An LCD holds whatever was last written to it. A frame that redraws only
    line 1 does not blank line 2 - line 2 keeps showing its previous content.
    So reporting only the lines present in one frame misrepresents the panel:
    it shows a one-line display that does not exist.

    This carries forward unredrawn lines, so every frame reports BOTH lines,
    which is what is actually on the glass.
    """

    def __init__(self):
        self.lines = [None, None]

    def update(self, slots):
        for slot, text in slots.items():
            self.lines[slot] = text
        return self.snapshot()

    def snapshot(self):
        return tuple("" if ln is None else ln for ln in self.lines)


def read_screen(runs, period):
    """Lines this frame redrew, in slot order. Prefer Display for reporting."""
    slots = read_slots(runs, period)
    return [slots[k] for k in sorted(slots)]


def parse_timeline(text):
    """Events in log order: ('mark', ts, text) and ('frame', ts, runs)."""
    events = []
    cur = None
    cur_ts = None
    for line in text.splitlines():
        line = line.rstrip("\r")
        ts_m = TS_RE.search(line)
        ts = ts_m.group(1) if ts_m else "--:--:--"

        mk = MARK_RE.search(line)
        if mk and "FRAME" not in line:
            events.append(("mark", ts, mk.group(1).strip()))
            continue

        if FRAME_RE.search(line):
            cur, cur_ts = [], ts
            continue
        if cur is None:
            continue
        if "=== END FRAME" in line:
            events.append(("frame", cur_ts, cur))
            cur = None
            continue
        body = line.split("]: ")[-1].strip()
        if body and all(PAIR_RE.fullmatch(t) for t in body.split()):
            for t in body.split():
                lvl, dur = t.split(":")
                cur.append((int(lvl), int(dur)))
    return events


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/modewalk_log.txt"
    with open(path, "r", errors="replace") as fh:
        text = fh.read()

    events = parse_timeline(text)
    frames = [e for e in events if e[0] == "frame"]
    marks = [e for e in events if e[0] == "mark"]
    print(f"{len(frames)} frames, {len(marks)} markers\n")
    if not frames:
        print("No frames parsed - check the log.")
        return

    # Fit T once over everything. The bit period is a property of the panel's
    # clock, not of the screen, so pooling all frames gives the best estimate.
    allruns = [r for _t, _ts, runs in frames for r in runs]
    period, err = best_period(allruns, lo=20.0, hi=32.0)
    print(f"bit period fitted over {len(allruns)} runs: "
          f"{period:.2f} us (error {err:.4f})\n")

    print("--- timeline (only screen CHANGES are printed) ---")
    last = None
    undecoded = 0
    seen = []
    panel = Display()
    for kind, ts, payload in events:
        if kind == "mark":
            print(f"[{ts}]  {payload}")
            last = None  # force a reprint of the screen after any marker
            continue
        slots = read_slots(payload, period)
        if not slots:
            undecoded += 1
            continue
        # Always report BOTH lines. A frame that redrew only one line leaves
        # the other one on the glass unchanged, so carrying it forward is the
        # honest reading - printing one line would claim the panel went blank.
        key = panel.update(slots)
        if key != last:
            last = key
            seen.append(key)
            shown = " / ".join(f"[{s}]" for s in key)
            print(f"[{ts}]      {shown}")

    print(f"\n--- summary ---")
    print(f"frames that failed to decode: {undecoded} / {len(frames)}")
    distinct = []
    for k in seen:
        if k not in distinct:
            distinct.append(k)
    print(f"distinct screens seen: {len(distinct)}")
    for k in distinct:
        joined = " ".join(k)
        hit = [e for e in EXPECTED if e in joined]
        flag = "OK " if hit else "?? "
        print(f"  {flag}" + " / ".join(f"[{s}]" for s in k)
              + (f"   <- matches {hit}" if hit else ""))

    # The actual test. Reduce the timeline to the sequence of MODE NAMES,
    # dropping menu screens and consecutive repeats, and compare it to the
    # cycle predicted before the run. Decoding plausible-looking text is not
    # enough; it has to be the RIGHT text in the RIGHT ORDER.
    observed = []
    for k in seen:
        joined = " ".join(k)
        for m in MODE_NAMES:
            if m in joined:
                if not observed or observed[-1] != m:
                    observed.append(m)
                break

    print("\n--- verdict ---")
    print(f"  predicted: {' -> '.join(PREDICTED)}")
    print(f"  observed : {' -> '.join(observed) if observed else '(nothing)'}")
    if observed == PREDICTED:
        print("  PASS - the decoder reproduced the full MODE cycle in order.")
    elif observed and all(m in MODE_NAMES for m in observed):
        print("  PARTIAL - all screens decoded to real mode names, but the "
              "sequence does not match the prediction.")
        print("  Check whether a press landed in the Time Mode menu, or "
              "whether the pump started somewhere other than Standby.")
    else:
        print("  FAIL - the predicted cycle did not come back.")


if __name__ == "__main__":
    main()
