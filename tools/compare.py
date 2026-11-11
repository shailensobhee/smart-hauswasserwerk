"""Compare two MODE-walk captures, and test the majority-vote mitigation.

Run 1 decoded 690/695 frames cleanly, with 5 single-bit slips. That number on
its own does not say whether the slips are:

  RANDOM   - timing jitter / ISR latency, independent between runs.
             Majority-voting across consecutive frames removes them.
  SYSTEMATIC - certain bit patterns in certain screens quantise badly.
             The SAME screen fails the SAME way every time, every frame is
             wrong identically, and voting does NOTHING because all three
             voters are wrong together.

That distinction decides whether the production recommendation is sound, and
it cannot be settled from one capture. Two independent runs can: if the corrupt
frames land on different screens at different character positions, the slips
are random.

This also actually RUNS the proposed mitigation over the real captures rather
than reasoning about it, because a mitigation that has not been run against
the data is a guess.

Usage:  python tools/compare.py <capture1> [<capture2> ...]
"""

import sys
from collections import Counter

from recover import best_period
from walk import MODE_NAMES, PREDICTED, parse_timeline, read_screen


def analyse(path):
    ev = parse_timeline(open(path, "r", errors="replace").read())
    frames = [e for e in ev if e[0] == "frame"]
    if not frames:
        return None
    allruns = [r for _t, _ts, runs in frames for r in runs]
    period, err = best_period(allruns, lo=20.0, hi=32.0)

    screens = []          # per frame: tuple of decoded lines
    for _t, _ts, runs in frames:
        screens.append(tuple(read_screen(runs, period)))

    corrupt = [(i, s) for i, s in enumerate(screens)
               if s and any("?" in ln for ln in s)]
    nomark = [i for i, s in enumerate(screens) if not s]

    # Observed mode sequence, for the pass/fail verdict.
    observed = []
    last = None
    for s in screens:
        if s == last:
            continue
        last = s
        joined = " ".join(s)
        for m in MODE_NAMES:
            if m in joined:
                if not observed or observed[-1] != m:
                    observed.append(m)
                break

    return {
        "path": path, "period": period, "err": err,
        "frames": frames, "screens": screens,
        "corrupt": corrupt, "nomark": nomark, "observed": observed,
    }


def majority_vote(screens, k=3):
    """Slide a window of k frames and pick the best screen in it.

    Plain majority is not quite right at a screen TRANSITION, where all k
    frames differ and there is no majority at all. Since a '?' is a
    self-identifying decode failure, prefer any clean candidate before falling
    back - that costs nothing and removes the residual that plain voting
    leaves sitting exactly where the display is changing.
    """
    out = []
    for i in range(len(screens) - k + 1):
        win = [s for s in screens[i:i + k] if s]
        if not win:
            out.append(())
            continue
        clean = [s for s in win if not any("?" in ln for ln in s)]
        pool = clean or win
        best, n = Counter(pool).most_common(1)[0]
        out.append(best if n >= 2 else pool[-1])
    return out


def main():
    paths = sys.argv[1:]
    if not paths:
        print("usage: compare.py <capture1> [<capture2> ...]")
        return

    results = []
    for p in paths:
        r = analyse(p)
        if r is None:
            print(f"{p}: no frames parsed")
            continue
        results.append(r)

        n = len(r["screens"])
        nc = len(r["corrupt"])
        print(f"=== {p}")
        print(f"  bit period     : {r['period']:.2f} us (err {r['err']:.4f})")
        print(f"  frames         : {n}")
        print(f"  clean          : {n - nc - len(r['nomark'])} "
              f"({(n-nc-len(r['nomark']))/n*100:.2f}%)")
        print(f"  bit-slipped    : {nc} ({nc/n*100:.2f}%)")
        print(f"  no marker      : {len(r['nomark'])}")
        print(f"  observed cycle : {' -> '.join(r['observed'])}")
        print(f"  verdict        : "
              f"{'PASS' if r['observed'] == PREDICTED else 'NOT MATCHING'}")

        if nc:
            print("  corrupt frames :")
            for i, s in r["corrupt"]:
                # Where does the text stop being right? That is the slip point.
                print(f"     #{i:4d}  " + " / ".join(f"[{x}]" for x in s))

        voted = majority_vote(r["screens"])
        vbad = [s for s in voted if s and any("?" in ln for ln in s)]
        print(f"  after 3-frame majority vote: {len(vbad)} bad / "
              f"{len(voted)} windows ({len(vbad)/max(len(voted),1)*100:.2f}%)")
        print()

    if len(results) < 2:
        return

    print("=== random or systematic? ===")
    #
    # NOTE ON A TEST THAT WAS WRONG HERE FIRST:
    # The obvious test - "did the same screen slip in both runs?" - is useless,
    # because the screens that slip are simply the screens shown in the most
    # frames. Standby is on screen for hundreds of frames, so at a ~1% random
    # error rate it is almost CERTAIN to slip in both runs. That test reports
    # "systematic" for data that is purely random. Base rates matter.
    #
    # The two tests that actually separate the hypotheses:
    #   1. PER-SCREEN ERROR RATE. Systematic means a given screen's bit pattern
    #      quantises badly, so ~100% of that screen's frames would be wrong.
    #      Random means every screen sits near the global rate.
    #   2. DO REPEATED FAILURES AGREE? Systematic corruption is deterministic:
    #      the same screen decodes to the same wrong text every time. Random
    #      corruption produces different garbage each time.
    #
    for r in results:
        print(f"\n  --- {r['path']}")
        per = {}
        for i, s in enumerate(r["screens"]):
            if not s:
                continue
            bad = any("?" in ln for ln in s)
            # Attribute a corrupt frame to its nearest preceding clean screen.
            key = s
            if bad:
                key = None
                for j in range(i - 1, -1, -1):
                    c = r["screens"][j]
                    if c and not any("?" in ln for ln in c):
                        key = c
                        break
                if key is None:
                    continue
            tot, nb, texts = per.get(key, (0, 0, []))
            per[key] = (tot + 1, nb + (1 if bad else 0),
                        texts + ([s] if bad else []))

        print(f"  {'screen':34s} {'frames':>7s} {'bad':>5s} {'rate':>7s}")
        for k, (tot, nb, _texts) in sorted(per.items(), key=lambda x: -x[1][0]):
            if nb or tot > 20:
                print(f"  {k[0][:32]:34s} {tot:7d} {nb:5d} "
                      f"{nb/tot*100:6.2f}%")

        # Test 2: did repeated failures of one screen produce identical garbage?
        print("  repeated failures of the same screen:")
        any_rep = False
        for k, (tot, nb, texts) in per.items():
            if nb >= 2:
                any_rep = True
                uniq = len(set(texts))
                print(f"    [{k[0][:30]}] failed {nb}x -> "
                      f"{uniq} DISTINCT wrong renderings")
                for t in texts:
                    print(f"        " + " / ".join(f"[{x}]" for x in t))
        if not any_rep:
            print("    (none repeated)")

    print("\n  INTERPRETATION:")
    print("  - If per-screen rates all sit near the global ~1%, and repeated")
    print("    failures of one screen produce DIFFERENT garbage each time,")
    print("    the slips are RANDOM timing noise and voting is valid.")
    print("  - Systematic would look like one screen at ~100% with identical")
    print("    wrong text every frame.")


if __name__ == "__main__":
    main()
