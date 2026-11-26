#!/usr/bin/env python3
"""bursty_feeder.py — feed known PCM into the daemon's FIFO in bursts.

Mimics the browser-mic jitter the playout buffer must absorb: a burst of
frames written back-to-back, then a pause (no bytes), repeated. Samples
increment (sample i has value (start_sample + i) & 0xFFFF) so
check_frames.py can verify frame content and order.

Default pattern (52 frames): 4 bursts of 13 frames, 400 ms pauses.
The bursts arrive faster than real-time; the pauses are the gaps.
With the playout buffer the daemon must release them at a steady
1 frame / 40 ms with no underrun; without it the output mirrors the
input bursts (13 frames at once, then 400 ms of nothing).

Exits 0 on BrokenPipeError (daemon went away mid-burst); exits 1 on
other errors (e.g. ENXIO: no reader).
"""
import argparse
import os
import struct
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fifo", required=True)
    ap.add_argument("--frames", type=int, default=52,
                    help="total frames to write (1280 B each)")
    ap.add_argument("--burst", type=int, default=13,
                    help="frames per burst (written back-to-back)")
    ap.add_argument("--pause", type=float, default=0.4,
                    help="seconds to sleep between bursts")
    ap.add_argument("--start-sample", type=int, default=0)
    args = ap.parse_args()

    fd = os.open(args.fifo, os.O_WRONLY)  # blocks until daemon holds read end
    sample = args.start_sample
    written_frames = 0
    try:
        while written_frames < args.frames:
            n = min(args.burst, args.frames - written_frames)
            for _ in range(n):
                buf = bytearray()
                for _ in range(640):
                    buf += struct.pack("<H", sample & 0xFFFF)
                    sample += 1
                try:
                    os.write(fd, bytes(buf))
                except BrokenPipeError:
                    print("bursty_feeder: BrokenPipe (daemon gone), "
                          "wrote %d frames" % written_frames,
                          file=sys.stderr)
                    return 0
                written_frames += 1
            if written_frames < args.frames and args.pause > 0:
                time.sleep(args.pause)
    finally:
        os.close(fd)
    print("bursty_feeder: done, wrote %d frames (%d bytes)"
          % (written_frames, written_frames * 1280))
    return 0


if __name__ == "__main__":
    sys.exit(main())
