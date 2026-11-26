# ao_shim — LD_PRELOAD audio-output shim for the Yi 1080p Home camera

Local build only. **Nothing here has been deployed to any camera.**
No camera was touched, no file was pushed to GitHub, nothing was
written to `/dev/ao`.

## What it is

`libao_shim.so` is an `LD_PRELOAD` shim for the camera's stock `./rmm`
process. rmm drives the speaker only via `ioctl()` on `/dev/ao`; the
shim watches rmm enable the AO channel, then feeds PCM audio it reads
from a FIFO into the speaker through the exact ioctl protocol proven by
the controlled tone test (2026-11-16 23:46 WIB, 25/25 frames, tone
heard by the user).

## Protocol (proven on the live camera — see `ao_shim.c` header)

- Audio ioctl: nr 5, `0xC0785805`, 120-byte struct, 16 kHz / 16-bit /
  mono, 640 samples (1280 bytes) per frame. Struct zeroed except
  `enBitwidth=1`, `enSoundmode=0`, `pVirAddr[0]` = PCM chunk pointer,
  `u32Len=1280`, `s32MilliSec=-1`. Driver reads PCM via `pVirAddr`.
- Activation triggers (observed, never issued by the shim):
  nr 1 `SetPubAttr 0x40245801`, nr 6 `EnableChn 0x5806` on the ao fd.
- Amp via `/dev/cpld_periph`: `0x7010` arg 0 = ON, `0x7011` arg 0 = OFF.
  Amp is OFF when idle.

## Design

- Intercepts `open` / `open64` / `openat` / `ioctl` / `close` (PLT).
  Forwarding uses **raw syscalls only** — no libc, no `dlsym`, no
  recursion (contrast `ipc_multiplex.c`, which needs libdl).
- Tracks the `/dev/ao` fd and the `/dev/cpld_periph` fd seen via
  `open`. If rmm never opens the CPLD device, the shim opens it itself
  at activation (rmm has no reason to touch the amp).
- Activates **only** after rmm itself enables an AO channel (nr 1 or
  nr 6 succeeding on the ao fd). The shim never configures the AO
  device (no SetPubAttr / Enable / EnableChn from us).
- On activation: `mkfifo /tmp/audio_in_fifo` (if absent) and spawns
  one reader thread (raw `clone()` + `mmap`'d stack — no pthread).
  The thread reads 16 kHz mono S16LE PCM from the FIFO in 1280-byte
  chunks and issues the real nr-5 ioctl on the ao fd. FIFO EOF
  (writer closed) → close and reopen; short reads accumulate to full
  chunks; a partial tail survives across reopen.
- Amp ON (`0x7010`) before the first frame of a burst; amp OFF
  (`0x7011`) after ~2 s with no FIFO data. Never left on indefinitely.
- rmm's own ioctls are forwarded first and promptly; shim work
  happens after the forward returns.
- Crash-safety: every return checked, pointers NULL-checked, no
  `abort()`/`exit()` of the host (only the cloned reader thread ever
  exits, and only itself); ioctl errors are counted and tolerated;
  a vanishing ao fd stops injection and turns the amp off.
- Debug logging to stderr only if `/tmp/ao_shim_debug` exists at
  activation time.
- **Call logging (diagnostic instrument, 2026-11-17):** every
  intercepted call is recorded to `/tmp/ao_shim_calls.log`, one line
  per call — `[000123] open("/dev/ao", flags=0x00000002, mode=0000)
  = 3 errno=0`. The log is opened once, lazily, on the first
  intercepted call (raw `open`, `O_CREAT|O_APPEND|O_WRONLY`, 0644);
  each entry is a single raw `write`, so it survives even if rmm dies
  right after the call. Format per call: function name, arguments
  (path/flags/mode for open, fd/request/arg for ioctl, fd for close),
  return value, errno after the call. On a success line the errno
  value is whatever stale value the process had — ignore it; only
  failure errno values are meaningful. Paths are truncated to 125
  chars + `...`; buffer contents are never logged. Entries are
  capped at 4096 (log stops silently after that). A logging failure
  (open/write fails, or a short write) is silently ignored and marks
  the log dead — logging never changes a return value and never
  crashes rmm. No blocking locks: the guard is an atomic
  test-and-set (`ldrex`/`strex`), so under thread contention an
  entry may be dropped, never corrupted. Known limitation: the log
  fd is an ordinary fd of the host process — if rmm ever closed it
  blindly (e.g. a close-all-fds sweep), the `close()` wrapper
  records a `!! log fd N closed by host` marker line before
  forwarding the close, then logging stops.

## Toolchain — why `-nostdlib`

The hisiv300 toolchain is unobtainable, so this is built with Debian
`arm-linux-gnueabi-gcc`. The camera runs **uClibc, not glibc**, so a
glibc-linked `.so` would not load under the camera's uClibc loader.
The build therefore uses `-nostdlib -shared -fPIC` with **only raw
syscalls internally**: no libc calls, no pthread, no malloc (static
BSS buffers), no division/modulo or 64-bit arithmetic (those would
pull in `__aeabi_*` libcalls). Verified: `readelf -d` shows **no
NEEDED entries**, `nm -u` shows exactly one undefined symbol
(`__errno_location`, H1 fix).

## Build

```sh
make            # builds libao_shim.so
make check      # readelf/nm verification (ARM ELF32 DYN, exports, no NEEDED, exactly one undef: `__errno_location`)
make test       # qemu-arm mock test (see below)
make clean
```

Built 2026-11-17 with `arm-linux-gnueabi-gcc (Ubuntu 13.3.0)` (logging-shim build):

| file | sha256 |
| --- | --- |
| `libao_shim.so` | `a13150434b65f8561f3230458199d6fea3fdb244365c93289bb87d5e22d330ef` |
| `ao_shim.c` | `726df7e3209c50141af7d52cb33c119e8f66b90021e90d29608214018a6fe64e` |

(`libao_shim.so` is 11524 bytes, ELF32, DYN, ARM, exports
`open`, `open64`, `openat`, `ioctl`, `close`, plus exactly one undefined
symbol `__errno_location` (H1 fix). `open` and `open64` now have
separate bodies — they log different function names, so the compiler
can no longer merge them.)

## Verification (honest status)

- **Compile:** clean, no warnings (`make`, see `test/qemu_summary.log`
  for the test run; build log in `/tmp/ao_shim_build.log`).
- **readelf:** ARM 32-bit shared object, exports the five symbols,
  zero NEEDED entries, exactly one undefined symbol
  (`__errno_location`, H1 fix) (`make check` passes).
- **qemu-arm mock test (11/11 checks pass):** `make test` runs the ARM
  mock program under `qemu-arm` with a pty bind-mounted over
  `/dev/ao` as a fake audio device, twice:
  - *Test-hook build* (`libao_shim_test.so`, TCGETS also activates):
    activation ran, FIFO created, reader thread spawned, 3840 bytes
    fed → 3 nr-5 attempts, each failing `errno=25` (ENOTTY — a pty
    has no such ioctl, expected), tolerated with no crash; FIFO
    EOF/reopen worked (second 1280-byte write accepted); thread
    count 3 (2 qemu baseline + 1 reader). Logs:
    `test/qemu_run1.log`.
  - *Production build* (negative control): TCGETS forwarded but did
    NOT activate — no FIFO, no extra thread, thread count 2 (qemu
    baseline). Logs: `test/qemu_run2.log`.
  - Two mock-test traps found and fixed along the way (documented in
    `test/mock_test.c`): Debian arm glibc redirects `ioctl()` to
    `__ioctl_time64` via an asm label, which bypassed the shim until
    the mock declared plain `ioctl` itself; and qemu-user itself
    contributes a baseline thread, so thread-count expectations had
    to be calibrated (baseline 2, not 1).
  - *Call logging (2026-11-17):* with the logging shim the same
    11/11 suite still passes, and `/tmp/ao_shim_calls.log` contains
    every intercepted call with correct arguments, caller-visible
    return values (`-1` on failure, never the raw `-errno`) and
    errno (e.g. `open64("/nonexistent-ao-shim-xyz", ...) = -1
    errno=2`, `close(-1) = -1 errno=9`). A first version logged the
    raw syscall return (`-2`/`-9`) instead of `-1` — caught by the
    mock log-content checks and fixed before commit. Two more
    mock-environment traps: Debian arm glibc also redirects `open()`
    to `open64`, so the log shows `open64` for the mock's opens
    (plain `open` on the camera's uClibc); and the `mode` argument
    of a 2-argument `open()` call is garbage in r2 (harmless —
    the kernel ignores it without `O_CREAT`).
- **NOT verified (and not claimed):** anything on the real camera.
  nr-5 success against the real AO driver, the amp on/off ioctls,
  audible sound, behaviour inside the real rmm (timing, fd reuse,
  interaction with Frigate/go2rtc), and the FIFO writer (not built
  yet — a future go2rtc backchannel or producer must feed 16 kHz
  mono S16LE PCM into `/tmp/audio_in_fifo`). Deployment needs a
  separate approval and a live test plan.

## Known limitations

- **errno is set on failure (H1 fix, 2026-11-17).** Intercepted
  `open`/`ioctl`/`close` call `__errno_location()` (exported by the
  camera's uClibc) and store the failed syscall's `-errno` there, so
  callers see the real errno. Stale errno was *suspected* to kill rmm
  in early startup, but foreground test 2 with this fix changed
  nothing (rmm still died in ~5.7 s), so H1 was refuted
  (2026-11-17). The old claim that errno could not be accessed from
  this `.so` was wrong.
- Only `open`/`open64`/`openat` are intercepted. If rmm opened
  `/dev/ao` another way (e.g. `__open_2`, or an fd inherited across
  exec), the ao fd would not be tracked and the shim would stay
  inert — safe, but silent.
- Activation keys off nr 1 / nr 6 succeeding on the ao fd. If a
  future rmm build enables the channel another way, the shim will
  not activate.
- A burst must eventually deliver complete 1280-byte chunks; a
  partial tail is held (not discarded) until more data arrives.
- The amp-off idle timer has 1-second resolution (raw `time()`
  syscall, chosen to avoid 64-bit arithmetic), so amp-off lands
  ~2–3 s after the last data, not exactly 2 s.

## Deployment (future — NOT done, needs separate approval)

Planned: in the camera's `system.sh`, launch rmm with
`LD_PRELOAD=/path/to/libao_shim.so` on its command line (the fork's
`DEPLOY.md` will carry the exact edit), and have the audio producer
(go2rtc backchannel or equivalent) write 16 kHz mono S16LE PCM to
`/tmp/audio_in_fifo`. No camera change is part of this build.

## Files

- `ao_shim.c` — the shim (protocol + design documented in its header)
- `Makefile` — build / check / test
- `libao_shim.so` — the production artifact (deployable later)
- `libao_shim_test.so` — TEST ARTIFACT ONLY (extra TCGETS trigger);
  never deploy this one
- `test/mock_test.c`, `test/run_qemu_test.sh` — qemu mock test
- `test/qemu_run1.log`, `test/qemu_run2.log`, `test/qemu_summary.log`
  — mock test evidence
