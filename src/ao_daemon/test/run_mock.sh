#!/bin/bash
#
# run_mock.sh — qemu mock suite for ao_daemon (Task 2).
#
# Runs all six Review Focus cases against the test-mode binary
# (ao_daemon_test, -DAO_DAEMON_TEST + -d) and prints PASS/FAIL per case.
#
#   Case 1: irregular chunk sizes -> frames exactly 1280 B, content in
#           order, no short frame (trailing partial < 1280 never sent).
#   Case 2: writer can open FIFO while daemon runs; fails ENXIO after
#           daemon killed.
#   Case 3: Nth SendFrame fails once -> daemon logs recovery, re-binds,
#           resumes (frames 4+ sent).
#   Case 4: SIGTERM mid-burst -> AMP OFF logged before exit, exit 0.
#   Case 5: writer disconnect, second burst later -> both delivered,
#           daemon still alive.
#   Case 6: burst ends mid-frame (partial tail) -> tail discarded on
#           disconnect; next burst starts clean at sample 0.
#
# Note: chunk size 7 in case 1 becomes 8 in the feeder (samples are
# 2 bytes; odd writes would break sample alignment of the checkable
# stream). 100/2048/8/3000 still exercises arbitrary write boundaries.
#
# Usage: ./run_mock.sh   (run from this directory; needs qemu-arm,
# python3, and ao_daemon_test built via ../compile.ao_daemon test)
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BIN="$SCRIPT_DIR/../ao_daemon_test"
FEEDER="$SCRIPT_DIR/mock_feeder.py"
CHECKER="$SCRIPT_DIR/check_frames.py"

MOCKDIR=$(mktemp -d /tmp/ao_mock.XXXXXX)
FIFO="$MOCKDIR/fifo"
RESULTS=()
DPID=""

cleanup() {
    if [ -n "$DPID" ] && kill -0 "$DPID" 2>/dev/null; then
        kill -TERM "$DPID" 2>/dev/null
        wait "$DPID" 2>/dev/null
    fi
    # Leave $MOCKDIR in place for inspection; caller removes it.
}
trap cleanup EXIT

pass() { RESULTS+=("PASS: $1"); echo "PASS: $1"; }
fail() { RESULTS+=("FAIL: $1"); echo "FAIL: $1"; }

# start_daemon <case-name> [extra env assignments...]
start_daemon() {
    local case="$1"; shift
    STUBLOG="$MOCKDIR/stub-$case.log"
    DLOG="$MOCKDIR/daemon-$case.log"
    rm -f "$FIFO" "$STUBLOG"
    env AO_DAEMON_FIFO="$FIFO" AO_DAEMON_TEST_LOG="$STUBLOG" "$@" \
        qemu-arm "$BIN" -d >"$DLOG" 2>&1 &
    DPID=$!
    # Wait for the daemon to create + hold the FIFO (up to 5 s).
    for _ in $(seq 1 50); do
        [ -p "$FIFO" ] && return 0
        sleep 0.1
    done
    echo "  (daemon did not create FIFO; log:)"
    cat "$DLOG"
    return 1
}

# stop_daemon — SIGTERM, wait, record exit code in STOP_RC.
# NOTE: must run in the MAIN shell, never inside $(...) — a bash
# command-substitution subshell cannot wait on the parent's background
# job and wait returns -1 there (verified 2026-11-17).
stop_daemon() {
    kill -TERM "$DPID" 2>/dev/null
    wait "$DPID" 2>/dev/null
    STOP_RC=$?
    DPID=""
}

# wait_for_frames <n> <stublog> <timeout-s>
wait_for_frames() {
    local want="$1" log="$2" timeout="$3" i=0
    while [ $i -lt $((timeout * 10)) ]; do
        local n
        n=$(grep -c '^FRAME [0-9]' "$log" 2>/dev/null || true)
        [ "${n:-0}" -ge "$want" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

echo "=== ao_daemon mock suite ==="
echo "mock dir: $MOCKDIR"
echo "binary:   $BIN"
[ -x "$BIN" ] || { echo "FAIL: $BIN missing — run ../compile.ao_daemon test first"; exit 1; }

# ---------------------------------------------------------------- case 1
echo "--- Case 1: irregular chunks -> 1280 B frames, in order ---"
if start_daemon case1; then
    python3 "$FEEDER" --fifo "$FIFO" --total 6500 --chunks 100,2048,7,3000
    if wait_for_frames 5 "$STUBLOG" 10; then
        sleep 3  # let the 2 s amp-idle timer fire
        if python3 "$CHECKER" --log "$STUBLOG" --expect-ok 1,2,3,4,5 \
                --expect-amp-on 1 --expect-amp-off 1; then
            pass "case 1 — 5 frames of exactly 1280 B, content in order, trailing 100 B partial never sent, AMP ON/OFF once each"
        else
            fail "case 1 — frame check failed (stub log: $STUBLOG)"
            cat "$STUBLOG"
        fi
    else
        fail "case 1 — timed out waiting for 5 frames (stub log: $STUBLOG)"
        cat "$STUBLOG"; cat "$DLOG"
    fi
    stop_daemon
    [ "$STOP_RC" = "0" ] || fail "case 1 — daemon exit code $STOP_RC (expected 0)"
else
    fail "case 1 — daemon did not start"
fi

# ---------------------------------------------------------------- case 2
echo "--- Case 2: writer open while running / ENXIO after kill ---"
if start_daemon case2; then
    if python3 -c "
import os, sys
fd = os.open('$FIFO', os.O_WRONLY | os.O_NONBLOCK)
os.write(fd, b'\x00\x00')
os.close(fd)
"; then
        stop_daemon
        if python3 -c "
import os, sys, errno
try:
    os.open('$FIFO', os.O_WRONLY | os.O_NONBLOCK)
except OSError as e:
    sys.exit(0 if e.errno == errno.ENXIO else 1)
sys.exit(1)
"; then
            pass "case 2 — writer opened FIFO while daemon ran; open fails ENXIO after SIGTERM (daemon exit $STOP_RC)"
        else
            fail "case 2 — writer open did NOT fail ENXIO after daemon killed"
        fi
    else
        fail "case 2 — writer could not open FIFO while daemon ran"
        stop_daemon
    fi
else
    fail "case 2 — daemon did not start"
fi

# ---------------------------------------------------------------- case 3
echo "--- Case 3: Nth SendFrame fails once -> recovery, resume ---"
if start_daemon case3 AO_DAEMON_TEST_FAIL_N=3; then
    python3 "$FEEDER" --fifo "$FIFO" --total 7680 --chunks 1280
    if wait_for_frames 6 "$STUBLOG" 10; then
        ok=1
        python3 "$CHECKER" --log "$STUBLOG" --expect-ok 1,2,4,5,6 \
            --expect-failed 3 || ok=0
        grep -q 'REBIND' "$STUBLOG" || { echo "  no REBIND in stub log"; ok=0; }
        grep -q 'recovery' "$DLOG" || { echo "  no recovery in daemon log"; ok=0; }
        if kill -0 "$DPID" 2>/dev/null; then
            [ $ok = 1 ] && pass "case 3 — frame 3 failed once, REBIND logged, frames 4-6 sent, daemon alive"
            [ $ok = 1 ] || fail "case 3 — frame/recovery check failed"
        else
            fail "case 3 — daemon died after injected failure"
        fi
        [ $ok = 0 ] && { echo "  stub log:"; cat "$STUBLOG"; echo "  daemon log:"; cat "$DLOG"; }
    else
        fail "case 3 — timed out waiting for 6 frame attempts"
        cat "$STUBLOG"; cat "$DLOG"
    fi
    stop_daemon
    [ "$STOP_RC" = "0" ] || fail "case 3 — daemon exit code $STOP_RC (expected 0)"
else
    fail "case 3 — daemon did not start"
fi

# ---------------------------------------------------------------- case 4
echo "--- Case 4: SIGTERM mid-burst -> AMP OFF before exit ---"
if start_daemon case4; then
    python3 "$FEEDER" --fifo "$FIFO" --total 128000 --chunks 1280 --delay 0.05 &
    FEEDER_PID=$!
    sleep 1  # mid-burst: amp ON, frames flowing
    grep -q '^AMP ON$' "$STUBLOG" || echo "  (warning: no AMP ON yet at SIGTERM time)"
    stop_daemon
    wait "$FEEDER_PID" 2>/dev/null
    if [ "$STOP_RC" = "0" ] && grep -q '^AMP OFF$' "$STUBLOG" \
            && grep -q 'shutdown' "$DLOG"; then
        # AMP OFF must come after AMP ON in the stub log.
        on_line=$(grep -n '^AMP ON$' "$STUBLOG" | head -1 | cut -d: -f1)
        off_line=$(grep -n '^AMP OFF$' "$STUBLOG" | head -1 | cut -d: -f1)
        if [ "$off_line" -gt "$on_line" ]; then
            pass "case 4 — SIGTERM mid-burst: exit 0, AMP OFF logged after AMP ON, shutdown line present"
        else
            fail "case 4 — AMP OFF not after AMP ON"
        fi
    else
        fail "case 4 — exit=$STOP_RC, AMP OFF/shutdown missing"
        echo "  stub log:"; cat "$STUBLOG"; echo "  daemon log:"; cat "$DLOG"
    fi
else
    fail "case 4 — daemon did not start"
fi

# ---------------------------------------------------------------- case 5
echo "--- Case 5: writer disconnect, second burst -> both delivered ---"
if start_daemon case5; then
    python3 "$FEEDER" --fifo "$FIFO" --total 3840 --chunks 1280
    sleep 1
    python3 "$FEEDER" --fifo "$FIFO" --total 2560 --chunks 1280 --start-sample 1920
    if wait_for_frames 5 "$STUBLOG" 10; then
        ok=1
        # Burst 2 starts at sample 1920, but the daemon's stream is
        # continuous: frame 4's first sample is 640*3 = 1920, matching
        # the feeder's --start-sample 1920. So plain order check works.
        python3 "$CHECKER" --log "$STUBLOG" --expect-ok 1,2,3,4,5 || ok=0
        if kill -0 "$DPID" 2>/dev/null; then
            [ $ok = 1 ] && pass "case 5 — burst 1 (3 frames) + burst 2 (2 frames) both delivered, daemon still alive"
            [ $ok = 1 ] || fail "case 5 — frame check failed"
        else
            fail "case 5 — daemon died after writer disconnect"
        fi
        [ $ok = 0 ] && { echo "  stub log:"; cat "$STUBLOG"; echo "  daemon log:"; cat "$DLOG"; }
    else
        fail "case 5 — timed out waiting for 5 frames"
        cat "$STUBLOG"; cat "$DLOG"
    fi
    stop_daemon
    [ "$STOP_RC" = "0" ] || fail "case 5 — daemon exit code $STOP_RC (expected 0)"
else
    fail "case 5 — daemon did not start"
fi

# ---------------------------------------------------------------- case 6
echo "--- Case 6: partial tail discarded on disconnect, next burst clean ---"
if start_daemon case6; then
    python3 "$FEEDER" --fifo "$FIFO" --total 3000 --chunks 1280
    sleep 1  # writer disconnects (feeder exits); daemon re-opens
    python3 "$FEEDER" --fifo "$FIFO" --total 2560 --chunks 1280 --start-sample 0
    if wait_for_frames 4 "$STUBLOG" 10; then
        sleep 3  # let the 2 s amp-idle timer fire
        ok=1
        grep -q '^FRAME 1 1280 00000100$' "$STUBLOG" || { echo "  FRAME 1 wrong"; ok=0; }
        grep -q '^FRAME 2 1280 80028102$' "$STUBLOG" || { echo "  FRAME 2 wrong"; ok=0; }
        # Without the discard fix FRAME 3 would start with the stale tail
        # (sample 1280: 00050105) and FRAME 4 with sample 420 (a401a501).
        grep -q '^FRAME 3 1280 00000100$' "$STUBLOG" || { echo "  FRAME 3 not a clean restart (stale tail spliced?)"; ok=0; }
        grep -q '^FRAME 4 1280 80028102$' "$STUBLOG" || { echo "  FRAME 4 wrong"; ok=0; }
        [ $ok = 1 ] && pass "case 6 — 440 B partial tail discarded on disconnect; burst 2 starts clean at sample 0"
        [ $ok = 1 ] || fail "case 6 — burst-2 frames not clean (stub log: $STUBLOG)"
        [ $ok = 0 ] && cat "$STUBLOG"
    else
        fail "case 6 — timed out waiting for 4 frames"
        cat "$STUBLOG"; cat "$DLOG"
    fi
    stop_daemon
    [ "$STOP_RC" = "0" ] || fail "case 6 — daemon exit code $STOP_RC (expected 0)"
else
    fail "case 6 — daemon did not start"
fi

echo "=== results ($MOCKDIR) ==="
for r in "${RESULTS[@]}"; do echo "  $r"; done
fails=$(printf '%s\n' "${RESULTS[@]}" | grep -c '^FAIL:' || true)
echo "mock dir kept at: $MOCKDIR"
if [ "$fails" = "0" ]; then
    echo "ALL CASES PASS"
    exit 0
else
    echo "$fails CASE(S) FAILED"
    exit 1
fi
