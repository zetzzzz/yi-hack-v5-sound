#!/usr/bin/env python3
"""check_frames.py — verify the AO stub log from a mock case.

Parses lines:
    FRAME <n> <bytes> <first-4-bytes-hex>   (successful SendFrame)
    FRAME <n> FAILED (injected)            (injected failure)
    AMP ON / AMP OFF                        (amp transitions)
    BIND ok (stub) / REBIND                 (bind events)

Checks for every successful FRAME n:
  - bytes == 1280 (no short frame, ever)
  - first two samples == (start_sample + 640*(n-1)) & 0xFFFF and +1
    (content in order; sample i of the stream is (start+i) & 0xFFFF)

Usage:
  check_frames.py --log STUBLOG --expect-ok 1,2,4,5,6 --expect-failed 3
                  [--start-sample N] [--expect-amp-on N] [--expect-amp-off N]
Exit 0 if all checks pass, 1 otherwise (prints FAIL lines).
"""
import argparse
import re
import struct
import sys

FRAME_RE = re.compile(r"^FRAME (\d+) (\d+) ([0-9a-f]{8})$")
FAILED_RE = re.compile(r"^FRAME (\d+) FAILED \(injected\)$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--expect-ok", default="",
                    help="comma-separated FRAME numbers expected ok")
    ap.add_argument("--expect-failed", default="",
                    help="comma-separated FRAME numbers expected failed")
    ap.add_argument("--start-sample", type=int, default=0)
    ap.add_argument("--expect-amp-on", type=int, default=None)
    ap.add_argument("--expect-amp-off", type=int, default=None)
    args = ap.parse_args()

    expect_ok = {int(x) for x in args.expect_ok.split(",") if x.strip()}
    expect_failed = {int(x) for x in args.expect_failed.split(",") if x.strip()}

    ok, failed = {}, set()
    amp_on = amp_off = 0
    problems = []

    with open(args.log) as f:
        for line in f:
            line = line.rstrip("\n")
            m = FRAME_RE.match(line)
            if m:
                n, nbytes, hx = int(m.group(1)), int(m.group(2)), m.group(3)
                if n in ok:
                    problems.append("duplicate FRAME %d" % n)
                ok[n] = (nbytes, hx)
                continue
            m = FAILED_RE.match(line)
            if m:
                failed.add(int(m.group(1)))
                continue
            if line == "AMP ON":
                amp_on += 1
            elif line == "AMP OFF":
                amp_off += 1

    for n, (nbytes, hx) in sorted(ok.items()):
        if nbytes != 1280:
            problems.append("FRAME %d: short frame %d bytes" % (n, nbytes))
        s0 = (args.start_sample + 640 * (n - 1)) & 0xFFFF
        s1 = (s0 + 1) & 0xFFFF
        want = struct.pack("<HH", s0, s1).hex()
        if hx != want:
            problems.append("FRAME %d: content %s, expected %s "
                            "(first samples %d,%d)" % (n, hx, want, s0, s1))

    if set(ok) != expect_ok:
        problems.append("ok frames %s, expected %s"
                        % (sorted(ok), sorted(expect_ok)))
    if failed != expect_failed:
        problems.append("failed frames %s, expected %s"
                        % (sorted(failed), sorted(expect_failed)))
    if args.expect_amp_on is not None and amp_on != args.expect_amp_on:
        problems.append("AMP ON count %d, expected %d"
                        % (amp_on, args.expect_amp_on))
    if args.expect_amp_off is not None and amp_off != args.expect_amp_off:
        problems.append("AMP OFF count %d, expected %d"
                        % (amp_off, args.expect_amp_off))

    if problems:
        for p in problems:
            print("FAIL: " + p)
        return 1
    print("OK: %d frames (%s), failed %s, AMP ON=%d OFF=%d"
          % (len(ok), sorted(ok), sorted(failed), amp_on, amp_off))
    return 0


if __name__ == "__main__":
    sys.exit(main())
