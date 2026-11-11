"""Verify the recovered decode against every captured frame.

recover.py found ONE hit by searching a parameter space, which is exactly the
situation where a coincidence is easy to mistake for a result. This locks the
parameters it found and asks the questions that a coincidence cannot survive:

  - does the SAME text fall out of all 8 independent frames?
  - does the second display line decode too, at the same settings?
  - does the full byte stream look like a display protocol, or like noise with
    one lucky English substring in it?

Usage:  python tools/verify.py <logfile>
"""

import sys

from recover import decode, parse_frames, render, to_bits

PERIOD = 25.50
WORD_BITS = 9
ALIGN = 0
MSB_FIRST = True


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/recover_log.txt"
    with open(path, "r", errors="replace") as fh:
        frames = parse_frames(fh.read())

    print(f"Locked: T={PERIOD} us, {WORD_BITS}-bit words, align {ALIGN}, "
          f"MSB first, marker 7C 40\n")

    texts = []
    for f in frames:
        bits = to_bits(f["runs"], PERIOD)
        by = decode(bits, WORD_BITS, ALIGN, MSB_FIRST)
        lines = render(by)
        span = sum(d for _l, d in f["runs"])
        print(f"FRAME {f['idx']}: {len(f['runs'])+1} edges, span {span} us, "
              f"{len(bits)} bits, {len(by)} words, {len(lines)} marker(s)")
        for pos, s in lines:
            print(f"    @word {pos:3d}: [{s}]")
        texts.append(tuple(s for _p, s in lines))

    print("\n--- reproducibility ---")
    uniq = set(texts)
    if len(uniq) == 1 and texts[0]:
        print(f"ALL {len(frames)} frames decode identically: {list(texts[0])}")
    else:
        print(f"{len(uniq)} distinct results across {len(frames)} frames:")
        for u in uniq:
            print(f"   {texts.count(u)}x {list(u)}")

    print("\n--- full word stream, frame 0 ---")
    bits = to_bits(frames[0]["runs"], PERIOD)
    by = decode(bits, WORD_BITS, ALIGN, MSB_FIRST)
    for i in range(0, len(by), 16):
        chunk = by[i:i + 16]
        hexs = " ".join(f"{b:02X}" for b in chunk)
        txt = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"  {i:3d}: {hexs:<48} |{txt}|")


if __name__ == "__main__":
    main()
