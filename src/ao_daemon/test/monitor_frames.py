#!/usr/bin/env python3
"""monitor_frames.py — timestamp FRAME lines as they appear in the AO stub log.

Polls the stub log every 5 ms and records time.monotonic() for each new
line matching ^FRAME. A silence (playout underrun filler) frame is
identified by its all-zero content (00000000). Prints the output
cadence: frame count, inter-frame gap stats, gaps > 100 ms, silence
count. The daemon's own log (not the stub log) carries the [underrun]
lines; check those separately.

Usage:
  monitor_frames.py --log STUBLOG --want N [--timeout S]
Exit 0 if N frames were seen, 1 on timeout (prints what was seen).
"""
import argparse
import re
import sys
import time

FRAME_RE = re.compile(r"^FRAME (\d+) (\d+) ([0-9a-f]{8})$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--want", type=int, required=True)
    ap.add_argument("--timeout", type=float, default=30.0)
    args = ap.parse_args()

    seen = []  # (monotonic_s, frame_no, is_silence)
    seen_nos = set()
    t0 = time.monotonic()
    pos = 0
    while time.monotonic() - t0 < args.timeout:
        try:
            with open(args.log, "rb") as f:
                f.seek(pos)
                data = f.read()
                pos = f.tell()
        except FileNotFoundError:
            data = b""
        if data:
            for line in data.decode(errors="ignore").splitlines():
                m = FRAME_RE.match(line)
                if m and int(m.group(1)) not in seen_nos:
                    seen_nos.add(int(m.group(1)))
                    seen.append((time.monotonic(),
                                 int(m.group(1)),
                                 m.group(3) == "00000000"))
            if len(seen) >= args.want:
                break
        time.sleep(0.005)

    print(f"frames seen: {len(seen)} (want {args.want})")
    if not seen:
        return 1
    sil = sum(1 for _, _, s in seen if s)
    print(f"silence frames: {sil}")
    gaps = []
    for i in range(1, len(seen)):
        gaps.append((seen[i][0] - seen[i - 1][0]) * 1000.0)
    if gaps:
        gaps_sorted = sorted(gaps)
        n = len(gaps_sorted)
        mean = sum(gaps) / n
        median = gaps_sorted[n // 2]
        mx = gaps_sorted[-1]
        over100 = sum(1 for g in gaps if g > 100)
        over200 = sum(1 for g in gaps if g > 200)
        print(f"inter-frame gaps (ms): n={n} mean={mean:.1f} "
              f"median={median:.1f} max={mx:.1f} "
              f">100ms={over100} >200ms={over200}")
        for i, g in enumerate(gaps):
            if g > 100:
                print(f"  gap {g:.0f} ms between FRAME {seen[i][1]} "
                      f"and {seen[i + 1][1]}")
    # First frame latency is measured by the caller (feeder start time).
    return 0 if len(seen) >= args.want else 1


if __name__ == "__main__":
    sys.exit(main())
