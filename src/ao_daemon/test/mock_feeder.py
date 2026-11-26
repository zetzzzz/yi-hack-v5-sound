#!/usr/bin/env python3
"""mock_feeder.py — feed known PCM into the daemon's FIFO in irregular chunks.

Writes a stream of incrementing uint16 LE samples (sample i has value
(start_sample + i) & 0xFFFF) so the mock checker can verify frame content
and order. Chunk sizes cycle through --chunks, mimicking the arbitrary
write boundaries of the real rRTSPServer sink (AAC 2048/1024 B, xLaw
dataSize*4 B).

Exits 0 on BrokenPipeError (daemon went away mid-burst — expected in the
SIGTERM case); exits 1 on other errors (e.g. ENXIO: no reader).
"""
import argparse
import os
import struct
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fifo", required=True)
    ap.add_argument("--total", type=int, required=True,
                    help="total bytes to write (must be even)")
    ap.add_argument("--chunks", default="1280",
                    help="comma-separated chunk sizes, cycled, e.g. 100,2048,7,3000")
    ap.add_argument("--delay", type=float, default=0.0,
                    help="seconds to sleep between chunks")
    ap.add_argument("--start-sample", type=int, default=0)
    args = ap.parse_args()

    if args.total % 2:
        print("feeder: --total must be even", file=sys.stderr)
        return 2

    sizes = [int(s) for s in args.chunks.split(",") if s.strip()]
    if not sizes or any(s <= 0 for s in sizes):
        print("feeder: bad --chunks", file=sys.stderr)
        return 2

    fd = os.open(args.fifo, os.O_WRONLY)  # blocks until daemon holds read end
    written = 0
    sample = args.start_sample
    ci = 0
    try:
        while written < args.total:
            n = min(sizes[ci % len(sizes)], args.total - written)
            ci += 1
            # Build n bytes (n is even only if all chunk sizes are even;
            # pad odd sizes to even so samples stay aligned).
            if n % 2:
                n += 1
                if written + n > args.total:
                    n -= 2
            buf = bytearray()
            for _ in range(n // 2):
                buf += struct.pack("<H", sample & 0xFFFF)
                sample += 1
            try:
                os.write(fd, bytes(buf))
            except BrokenPipeError:
                print("feeder: BrokenPipe (daemon gone), wrote %d bytes"
                      % written, file=sys.stderr)
                return 0
            written += len(buf)
            if args.delay:
                time.sleep(args.delay)
    finally:
        os.close(fd)
    print("feeder: done, wrote %d bytes" % written)
    return 0


if __name__ == "__main__":
    sys.exit(main())
