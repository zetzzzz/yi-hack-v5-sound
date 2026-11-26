# DEPLOY.md — deploy the two-way-audio fork to cctvkamar

> **STATUS: PLAN ONLY. NOT DEPLOYED. DO NOT RUN THE STEPS BELOW UNTIL
> ZU APPROVES THE DEPLOY IN A SEPARATE DECISION.**
>
> This document is the exact deploy plan for the fork built in this repo
> (yi-hack-v5 0.4.1 + RTSP audio backchannel + `libao_shim.so`).
> Deploying changes the camera's software; a bad deploy can break the
> camera until the SD card is fixed by hand. Read the whole document
> before starting, and keep the rollback files from step 1.

## Camera layout (verified live 2026-11-17 — read this first)

The camera has TWO yi-hack-v5 trees and they are NOT the same. The
original version of this document got this wrong (it used
`/home/yi-hack-v5/` everywhere); the failed deploy of 2026-11-17
proved the correct layout below.

| Tree | Where | Contents |
|---|---|---|
| **Live tree (SD card)** | `/tmp/sd/yi-hack-v5/` (vfat, `/dev/mmcblk0p1` mounted on `/tmp/sd`) | `bin/`, `www/`, `etc/`, `script/` — the binaries and web files the camera actually runs. `/proc/<pid>/exe` for rRTSPServer points here. |
| **Partial mirror (internal flash)** | `/home/yi-hack-v5/` (jffs2, `/dev/mtdblock5` mounted on `/home`) | `etc/`, `lib/`, `script/`, `version` only. **NO `bin/`, NO `www/`.** |

Consequences:
- Binaries (`rRTSPServer`, `pcmvol`) and web files (CGI, `speak.html`,
  `speak.js`) go to the **SD tree** (`/tmp/sd/yi-hack-v5/...`).
- `libao_shim.so` goes to **`/home/yi-hack-v5/lib/`** — that is where
  rmm's `LD_LIBRARY_PATH` points and where the `LD_PRELOAD` absolute
  path in `system.sh` points.
- `system.sh` is **identical in both trees** (stock md5
  `d63a8c5c6e4caf612a090db91bf60642`) — update **both** copies.
- `system.conf` live is **`/tmp/sd/yi-hack-v5/etc/system.conf`**
  (stock md5 `34c97d8164b5705b6f64bdbdc7dd6c82`); the `/home` copy is
  stale and must not be edited.
- Staging goes to **`/tmp/sd/deploy-staging/`**, not `/tmp` (RAM is tight).
- **`sha256sum` does NOT exist on the camera** — verify uploads with
  `md5sum` (md5 values are listed in the table below).

## What is being deployed (and why)

| Component | File in this repo (build output) | On-camera destination | Purpose |
|---|---|---|---|
| RTSP backchannel server | `build/home/yi-hack-v5/bin/rRTSPServer` (sha256 `33d42f59…`, md5 `6ab80d3b8547bf126d97a4cb0ca9f15a`, 1,697,644 B) | `/tmp/sd/yi-hack-v5/bin/rRTSPServer` | RTSP server that accepts an audio backchannel (sendonly) and writes decoded 16 kHz mono S16LE PCM to `/tmp/audio_in_fifo` |
| Audio shim | `build/home/yi-hack-v5/lib/libao_shim.so` (sha256 `3d0999b2…`, md5 `b391854f5719c99237c01fc4d40f44e7`, 6,420 B) | `/home/yi-hack-v5/lib/libao_shim.so` | LD_PRELOAD shim inside `./rmm`: reads PCM from `/tmp/audio_in_fifo` and injects it into the speaker via the proven ioctl nr 5 protocol, amp on/off via cpld |
| Volume tool | `build/home/yi-hack-v5/bin/pcmvol` (sha256 `824202b1…`, md5 `0b52e4b15747a64f2d12edc4ecc631e0`, 517,312 B) | `/tmp/sd/yi-hack-v5/bin/pcmvol` | PCM gain helper used by the speaker CGI scripts |
| Speaker CGI | `src/www/httpd/cgi-bin/speaker.sh`, `speak.sh`, `speaker_file.sh` | `/tmp/sd/yi-hack-v5/www/cgi-bin/` | WebUI speaker endpoints (play WAV/file, TTS) |
| Speaker WebUI | `src/www/httpd/htdocs/pages/speak.html`, `src/www/httpd/htdocs/js/modules/speak.js` | `/tmp/sd/yi-hack-v5/www/htdocs/pages/`, `.../htdocs/js/modules/` | "Speak" page in the camera WebUI (nav entry added after PTZ) |
| Launch wiring | `src/static/static/home/yi-hack-v5/script/system.sh` (rmm lines now carry `LD_PRELOAD=/home/yi-hack-v5/lib/libao_shim.so`) | `/tmp/sd/yi-hack-v5/script/system.sh` **AND** `/home/yi-hack-v5/script/system.sh` (both copies, they are identical) | Preloads the shim into `./rmm` on both cloud-on and cloud-off branches |
| Config | `RTSP_AUDIO_BC=aac` (new key, default `aac`) in `system.conf` | `/tmp/sd/yi-hack-v5/etc/system.conf` | Backchannel codec: `aac` (default), `alaw`, `ulaw`, or `none` |

Audio chain after deploy: HA WebRTC call → go2rtc `cctvkamar_talk`
(`#backchannel=1`, already configured) → camera RTSP backchannel →
`ADTS2PCMFileSink` decodes to 16 kHz mono S16LE → `/tmp/audio_in_fifo`
→ `libao_shim.so` inside `rmm` → ioctl nr 5 to `/dev/ao`, amp on
(`0x7010`) before / off (`0x7011`) after.

## Prerequisites

- Camera reachable: `192.168.10.42`, SSH root, empty password, legacy
  algorithms (`-o HostKeyAlgorithms=ecdsa-sha2-nistp256
  -o MACs=hmac-sha2-256,hmac-sha1`). Helper on the VM:
  `~/workspace/yi-ssh.py` (paramiko over the egress proxy).
- Build the firmware tree first (from this repo root):
  `./scripts/compile.sh` (or per-module `./src/<mod>/compile.<mod>` +
  `./src/<mod>/install.<mod>` for `rRTSPServer`, `pcmvol`, `ao_shim`,
  `www`). Verify the three hashes above before uploading.
- **Full `compile.www` was NOT run on this VM** (yui-compressor/xmllint
  not installed, so the compressed www bundle was not built). The
  `speak.html`/`speak.js` sources are in the repo and are picked up
  automatically when `compile.www` runs; if deploying without a full
  www build, copy those two files by hand to the paths above.
- Zu is at home and can listen at the camera (cctvkamar, bedroom).

## Deploy steps

### 1. Back up the camera's current files (do not skip)
```sh
ssh -o HostKeyAlgorithms=ecdsa-sha2-nistp256 -o MACs=hmac-sha2-256,hmac-sha1 root@192.168.10.42
mkdir -p /tmp/sd/backup-20261116
cp /tmp/sd/yi-hack-v5/bin/rRTSPServer /tmp/sd/backup-20261116/rRTSPServer.orig
cp /tmp/sd/yi-hack-v5/script/system.sh /tmp/sd/backup-20261116/system.sh.orig
cp /home/yi-hack-v5/script/system.sh /tmp/sd/backup-20261116/system.sh.home.orig
cp /tmp/sd/yi-hack-v5/etc/system.conf /tmp/sd/backup-20261116/system.conf.orig
cp /tmp/sd/yi-hack-v5/bin/pcmvol /tmp/sd/backup-20261116/pcmvol.orig 2>/dev/null || echo "pcmvol not present (expected on stock v5)"
ls -l /tmp/sd/backup-20261116/
```
Keep this backup until the call test in step 5 passes.

### 2. Upload the new files (from the VM, via scp)
Stage into `/tmp/sd/deploy-staging/` (NOT `/tmp` — RAM is tight), and
verify each file with `md5sum` on the camera right after upload
(`sha256sum` does not exist on the camera). The camera's Dropbear has
no SFTP/SCP subsystem, so piping through the SSH exec channel
(`cat > <file>`, chunked sends + explicit EOF) is the proven upload
route — see the tone-test upload script for the working pattern; the
buffered `makefile('wb')` approach stalls.
```sh
# from the repo root on the VM; adjust the scp route if the egress proxy is down
# (stage everything into /tmp/sd/deploy-staging/ on the camera first)
scp -o HostKeyAlgorithms=ecdsa-sha2-nistp256 -o MACs=hmac-sha2-256,hmac-sha1 \
  build/home/yi-hack-v5/bin/rRTSPServer build/home/yi-hack-v5/bin/pcmvol \
  build/home/yi-hack-v5/lib/libao_shim.so \
  root@192.168.10.42:/tmp/sd/deploy-staging/
scp -o HostKeyAlgorithms=ecdsa-sha2-nistp256 -o MACs=hmac-sha2-256,hmac-sha1 \
  src/www/httpd/cgi-bin/speaker.sh src/www/httpd/cgi-bin/speak.sh \
  src/www/httpd/cgi-bin/speaker_file.sh \
  root@192.168.10.42:/tmp/sd/deploy-staging/
scp -o HostKeyAlgorithms=ecdsa-sha2-nistp256 -o MACs=hmac-sha2-256,hmac-sha1 \
  src/www/httpd/htdocs/pages/speak.html src/www/httpd/htdocs/js/modules/speak.js \
  root@192.168.10.42:/tmp/sd/deploy-staging/
# also stage the new system.sh:
scp -o HostKeyAlgorithms=ecdsa-sha2-nistp256 -o MACs=hmac-sha2-256,hmac-sha1 \
  src/static/static/home/yi-hack-v5/script/system.sh \
  root@192.168.10.42:/tmp/sd/deploy-staging/
# verify md5s of the staged files (expected md5s are in the table above):
#   rRTSPServer 6ab80d3b8547bf126d97a4cb0ca9f15a
#   pcmvol      0b52e4b15747a64f2d12edc4ecc631e0
#   libao_shim  b391854f5719c99237c01fc4d40f44e7
#   system.sh   e5d9a1a9af6055232a9240ecc53150d6
```

### 3. Install on the camera (SSH) and verify hashes
```sh
ssh -o HostKeyAlgorithms=ecdsa-sha2-nistp256 -o MACs=hmac-sha2-256,hmac-sha1 root@192.168.10.42
cp /tmp/sd/deploy-staging/rRTSPServer /tmp/sd/yi-hack-v5/bin/rRTSPServer
cp /tmp/sd/deploy-staging/pcmvol /tmp/sd/yi-hack-v5/bin/pcmvol
cp /tmp/sd/deploy-staging/libao_shim.so /home/yi-hack-v5/lib/libao_shim.so
cp /tmp/sd/deploy-staging/speaker.sh /tmp/sd/deploy-staging/speak.sh /tmp/sd/deploy-staging/speaker_file.sh /tmp/sd/yi-hack-v5/www/cgi-bin/
cp /tmp/sd/deploy-staging/speak.html /tmp/sd/yi-hack-v5/www/htdocs/pages/
cp /tmp/sd/deploy-staging/speak.js /tmp/sd/yi-hack-v5/www/htdocs/js/modules/
cp /tmp/sd/deploy-staging/system.sh /tmp/sd/yi-hack-v5/script/system.sh
cp /tmp/sd/deploy-staging/system.sh /home/yi-hack-v5/script/system.sh
chmod +x /tmp/sd/yi-hack-v5/bin/rRTSPServer /tmp/sd/yi-hack-v5/bin/pcmvol
chmod +x /tmp/sd/yi-hack-v5/www/cgi-bin/speaker.sh /tmp/sd/yi-hack-v5/www/cgi-bin/speak.sh /tmp/sd/yi-hack-v5/www/cgi-bin/speaker_file.sh
# set the backchannel codec (default aac) if not already present:
grep -q '^RTSP_AUDIO_BC=' /tmp/sd/yi-hack-v5/etc/system.conf || echo 'RTSP_AUDIO_BC=aac' >> /tmp/sd/yi-hack-v5/etc/system.conf
# verify hashes BEFORE restarting anything (md5sum — sha256sum does NOT
# exist on the camera):
md5sum /tmp/sd/yi-hack-v5/bin/rRTSPServer /tmp/sd/yi-hack-v5/bin/pcmvol /home/yi-hack-v5/lib/libao_shim.so
# expected md5: rRTSPServer 6ab80d3b8547bf126d97a4cb0ca9f15a /
# pcmvol 0b52e4b15747a64f2d12edc4ecc631e0 /
# libao_shim.so b391854f5719c99237c01fc4d40f44e7
# (compare the full strings, not prefixes)
grep -n 'LD_PRELOAD.*libao_shim.so.*./rmm' /tmp/sd/yi-hack-v5/script/system.sh /home/yi-hack-v5/script/system.sh
# Both system.sh copies must show the two rmm lines carrying the
# LD_PRELOAD prefix. If they do not, DO NOT restart — fix first.
```

### 4. Restart `rmm` (and rRTSPServer)
```sh
# rmm is watched by watch_process; killing it restarts it with the new
# LD_PRELOAD. rRTSPServer is (re)started by wd_rtsp.sh / system.sh flow.
killall rmm
sleep 10
ps | grep -E 'rmm|rRTSPServer'
# rmm must be running again; rRTSPServer must be running with -b (check its
# command line in ps output). If rmm does not come back within 30 s,
# reboot the camera:  busybox reboot
# NEVER remove the SD card: without it the camera boots only to telnet+FTP.
```

### 5. Verify (in order — stop at the first failure, do not guess)
1. **RTSP backchannel offered:** from the HA host (or VM), DESCRIBE
   `rtsp://192.168.10.42/ch0_1.h264` — the SDP must contain an audio
   `m=` line with `a=sendonly` (AAC `mpeg4-generic/16000`, or
   PCMA/PCMU for alaw/ulaw). (QEMU-tested pre-deploy: aac/ulaw/alaw all
   returned `200 OK` with a sendonly audio track.)
2. **Live video still works:** HA `camera.cctv_kamar` shows live video;
   Frigate person detection still produces events.
3. **FIFO appears on a call:** start a 2-way call from the HA "CCTV Talk"
   dashboard, then on the camera check `ls -l /tmp/audio_in_fifo`
   exists while the call is active.
4. **Voice test (Zu listens at the camera):** during the call, tap the
   mic and speak. Zu must hear the voice from the camera speaker.
   This is the only proof the end-to-end chain works.

### 6. Rollback (if anything in step 5 fails)
```sh
ssh -o HostKeyAlgorithms=ecdsa-sha2-nistp256 -o MACs=hmac-sha2-256,hmac-sha1 root@192.168.10.42
cp /tmp/sd/backup-20261116/rRTSPServer.orig /tmp/sd/yi-hack-v5/bin/rRTSPServer
cp /tmp/sd/backup-20261116/system.sh.orig /tmp/sd/yi-hack-v5/script/system.sh
cp /tmp/sd/backup-20261116/system.sh.home.orig /home/yi-hack-v5/script/system.sh
cp /tmp/sd/backup-20261116/system.conf.orig /tmp/sd/yi-hack-v5/etc/system.conf
rm /home/yi-hack-v5/lib/libao_shim.so /tmp/sd/yi-hack-v5/bin/pcmvol
rm /tmp/sd/yi-hack-v5/www/cgi-bin/speaker.sh /tmp/sd/yi-hack-v5/www/cgi-bin/speak.sh /tmp/sd/yi-hack-v5/www/cgi-bin/speaker_file.sh
rm /tmp/sd/yi-hack-v5/www/htdocs/pages/speak.html /tmp/sd/yi-hack-v5/www/htdocs/js/modules/speak.js
killall rmm; sleep 10; ps | grep -E 'rmm|rRTSPServer'
# then verify live video + Frigate detection are back to normal.
```

## Known limitations (unverified until the on-camera test in step 5)
- Full SETUP/PLAY + RTP audio → shim → speaker flow has never run on the
  camera; only the pieces were verified (tone test for the shim protocol,
  QEMU DESCRIBE for the backchannel SDP).
- Shim limitation: intercepted `open`/`ioctl`/`close` return -1 on error
  but leave `errno` stale (no uClibc TLS errno access from a `-nostdlib`
  .so). Callers that check only `ret < 0` are unaffected.
- `speak.sh` TTS needs `nanotts`, which is not in v5 and was not ported —
  TTS will report "TTS engine not found" until nanotts is ported.
- Not ported (deliberate): `YiNoiseReduction` (Allwinner `IaaApc_*`,
  chipset-specific), MStar forward-audio WAV/H26x (v5 stays AAC-only).
- Backchannel presence in the built binary was verified via strings
  (`ADTS2PCMFileSink`, `OnDemandServerMediaSubsession_BC`,
  `/tmp/audio_in_fifo`) and QEMU DESCRIBE tests — not via `nm`, because
  the binary is stripped (`nm` shows no symbols on a stripped binary).
