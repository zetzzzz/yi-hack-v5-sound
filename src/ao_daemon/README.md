# ao_daemon — standalone speaker daemon (no LD_PRELOAD)

Reads decoded S16LE 16 kHz mono PCM from `/tmp/audio_in_fifo` (written by
the fork rRTSPServer's backchannel sink) and injects it into the camera
speaker via `/dev/ao`. No LD_PRELOAD, no touching `rmm`.

## Interface

| Item | Value |
|---|---|
| FIFO path | `/tmp/audio_in_fifo` (env `AO_DAEMON_FIFO` overrides — mock only) |
| Input | S16LE 16 kHz mono byte stream, arbitrary write boundaries |
| Frame size | exactly **1280 B** (640 samples, 40 ms); short frames are never sent |
| Amp policy | ON before the first frame of a burst; OFF after 2 s idle |
| Signals | SIGTERM / SIGINT → amp OFF (if on), shutdown line, exit 0 |
| Logging | stderr, timestamped; open/bind/ready/amp transitions, per-burst frame counts, errors. `-v` adds per-frame lines. |

## Flags / env

- `-v` — per-frame logging.
- `-d` — test-mode AO stub (only in a `-DAO_DAEMON_TEST` build; a normal
  build rejects `-d` with exit 2).
- `AO_DAEMON_FIFO` — FIFO path override (used by the mock).
- `AO_DAEMON_TEST_LOG` — (test build only) file the AO stub appends to:
  `FRAME <n> <bytes> <first-4-bytes-hex>` per SendFrame,
  `FRAME <n> FAILED (injected)` on an injected failure,
  `AMP ON` / `AMP OFF` per amp transition, `BIND ok (stub)` / `REBIND`.
  Defaults to stderr if unset.
- `AO_DAEMON_TEST_FAIL_N` — (test build only) fail the Nth SendFrame once,
  to exercise the recovery path.

## Build

```sh
./compile.ao_daemon        # ao_daemon        — real /dev/ao ioctls
./compile.ao_daemon test   # ao_daemon_test   — AO ioctl layer stubbed
```

`arm-linux-gnueabi-gcc` 13.3.0, `-static` (camera runs uClibc; a glibc
binary would not run there). Normal libc calls only — no raw syscalls,
no interposition, no LD_PRELOAD.

## Proven ioctl sequence reused (ao_tone_test.c, 23:46 WIB 2026-11-16)

open `/dev/ao` O_RDWR → ioctl nr 0 `0x40045800` bind id=0 → ioctl nr 27
`0x8004581B` query ready (must be 1; retry 500 ms up to ~30 s, then every
5 s forever, logging state changes — covers the boot race where `rmm`
has not finished AO init) → open `/dev/cpld_periph` O_RDWR →
ioctl `0x7010` arg 0 amp ON / `0x7011` arg 0 amp OFF → ioctl nr 5
`0xC0785805` SendFrame, 120-byte struct: enBitwidth=1, enSoundmode=0,
pVirAddr[0]=frame buffer, u32Len=1280, s32MilliSec=-1, rest zeroed.

The daemon does NOT configure AO itself: it relies on `rmm`'s
already-configured channel, so it only works while `rmm` is running
(which is always in normal operation).

## Failure handling

- **SendFrame fails** (e.g. `rmm` restarted, AO channel invalidated):
  log ret/errno, close `/dev/ao`, re-open, re-bind (nr 0), re-query
  (nr 27, must be 1), resume. Never exits on SendFrame failure. The
  failed frame is dropped.
- **Writer disconnect** (`read()` returns 0): close and re-open the FIFO
  read end, keep running. Leftover bytes (< 1280) from the ended burst
  are DISCARDED — the next writer is a fresh PCM stream (one sink per
  call), so keeping the old tail would splice stale audio into the new
  call's first frame. Within a burst, leftover bytes stay buffered for
  the next read.
- **SIGTERM / SIGINT** while waiting for nr 27 ready exits promptly
  (exit 0); a failed FIFO re-open is retried ~once per second instead
  of leaving the daemon deaf.
- **SIGKILL** cannot be handled: the amp may be left ON. The foreground
  test kills with plain `kill` (= SIGTERM), which is handled.

## Playout (jitter) buffer (2026-11-26)

Complete frames are queued and released one per 40 ms on a monotonic
clock, after a prefill of 10 frames (400 ms). The browser mic / phone
Wi-Fi delivers frames in bursts with occasional 200–400 ms gaps, and
the 5-frame AO buffer (200 ms) alone cannot absorb them; the playout
buffer absorbs gaps up to ~400 ms (+ 200 ms AO = ~600 ms total) at the
cost of ~400 ms added latency (fine for walkie-talkie). It does NOT
fix gaps > ~600 ms.

- Prefill: playout starts when the queue reaches 10 frames, or 1000 ms
  after the first frame arrived (whichever comes first).
- Underrun (queue empty at a release tick): logged at the DEFAULT level
  with a timestamp (`[underrun] run #N started/ended`), and a 1280-byte
  silence frame is sent so the AO buffer stays fed. The underrun count
  is the measurable dropout metric (the daemon runs without `-v`).
  A run of 50 consecutive underrun ticks (2.0 s) stops playout — that
  is a pause, not jitter — and the amp idles off as usual.
- Queue cap 25: beyond it the oldest frame is dropped (latency
  protection) and logged (`[playout] queue full ... dropped oldest`).
- Writer disconnect: the queued tail is played out, then playout stops
  (no silence after a disconnect). The verbose `[frame]` line marks
  silence frames (`[frame] #N silence sent`) so the existing
  `[frame] #N sent` parsers keep counting audio frames only.

Verified: qemu mock case 7 (bursty input, 4 bursts of 13 frames with
400 ms pauses) — NEW binary: 52/52 frames, output gaps mean 40.0 ms /
max 57.2 ms, 0 underruns; OLD binary (control): same input, output gaps
max 402.7 ms. Live camera bursty test through the production
backchannel: 52/52 audio frames, 0 silence, 0 underruns, output gaps
mean 40.1 ms / max 81.0 ms.

## What the mock (Task 2) does NOT cover

The real driver path — `/dev/ao` nr-5 SendFrame against the live
HiSilicon AO channel and the actual speaker. That path was already
proven by the tone test (25/25 frames, Zu heard the 1 kHz tone).
The mock proves everything around it: chunking, FIFO hold-open/ENXIO
behaviour, recovery, amp-off-on-kill, writer-disconnect handling, and
discarding a partial tail on disconnect.
