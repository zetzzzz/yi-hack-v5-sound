/*
 * ao_daemon.c — Standalone speaker daemon for the Yi 1080p Home camera.
 *
 * Reads decoded S16LE 16 kHz mono PCM from /tmp/audio_in_fifo (written by
 * the fork rRTSPServer's backchannel sink) and injects it into the camera
 * speaker via /dev/ao — with NO LD_PRELOAD and NO touching rmm.
 *
 * Proven ioctl sequence (ao_tone_test.c, 23:46 WIB 2026-11-16, Zu heard
 * the 1 kHz tone; exit 0, 25/25 frames, rmm untouched):
 *   open /dev/ao O_RDWR
 *   ioctl nr 0  (0x40045800) bind, arg &id, id=0
 *   ioctl nr 27 (0x8004581B) query ready, must return 1
 *   open /dev/cpld_periph O_RDWR
 *   ioctl 0x7010 arg 0 = amp ON, 0x7011 arg 0 = amp OFF
 *   ioctl nr 5  (0xC0785805) SendFrame, 120-byte struct (see below)
 *
 * nr-5 struct (120 B, zeroed, only these fields):
 *   +0   enBitwidth  = 1 (u32)
 *   +4   enSoundmode = 0 mono (u32)
 *   +8   pVirAddr[0] = pointer to the 1280-byte PCM frame buffer (u32)
 *   +36  u32Len      = 1280 (u32)
 *   +112 s32MilliSec = -1 (i32, infinite timeout — driver blocks until the
 *        5-frame buffer has room, which paces the daemon naturally)
 *
 * Input contract (fork rRTSPServer): S16LE 16 kHz mono byte stream with
 * ARBITRARY write boundaries (AAC frame decodes to 2048 B, or 1024 B if
 * SBR downsample; xLaw path writes dataSize*4 B). The daemon accumulates
 * and re-chunks into exactly 1280-byte frames (640 samples). A short frame
 * to nr 5 corrupts audio — never send one.
 *
 * FIFO contract: rRTSPServer's sink opens the FIFO O_WRONLY|O_NONBLOCK and
 * fails ENXIO if no reader exists, so the daemon creates the FIFO
 * (mkfifo) and holds the read end open permanently, before any call.
 *
 * Amp policy: ON before the first frame of a burst, OFF after 2 s idle.
 * SIGTERM/SIGINT: amp OFF (if on), close fds, shutdown line, exit 0.
 *
 * Playout (jitter) buffer (2026-11-26): complete frames are queued and
 * released one per 40 ms on a monotonic clock, after a prefill of
 * PLAYOUT_DEPTH frames (400 ms). This smooths input jitter — the browser
 * mic / phone Wi-Fi delivers frames in bursts with occasional 200-400 ms
 * gaps, and the 5-frame AO buffer (200 ms) alone cannot absorb them.
 * The playout buffer absorbs gaps up to ~400 ms (+ 200 ms AO = ~600 ms
 * total) at the cost of ~400 ms added latency (fine for walkie-talkie).
 * Underrun (queue empty at a release tick): logged at the DEFAULT level
 * with a timestamp, and a 1280-byte silence frame is sent so the AO
 * buffer stays fed. A run of 50 consecutive underrun ticks (2.0 s) stops
 * playout — that is a pause, not jitter — and the amp idles off as
 * usual. Queue cap PLAYOUT_CAP: beyond it the oldest frame is dropped
 * (latency protection). On writer disconnect the queued tail is played
 * out, then playout stops (no silence after a disconnect).
 *
 * SendFrame failure (e.g. rmm restarted, AO channel invalidated): log,
 * close /dev/ao, re-open, re-bind (nr 0), re-query (nr 27, must be 1),
 * resume. Never exit on SendFrame failure.
 *
 * Writer disconnect (read() returns 0): close and re-open the FIFO read
 * end, keep running — and DISCARD any leftover partial frame (< 1280 B):
 * the writer is one call's sink, so the next writer is a fresh PCM
 * stream; keeping the old tail would splice stale audio into the new
 * call's first frame.
 *
 * Build: see compile.ao_daemon. Normal build talks to the real devices.
 * Test build (-DAO_DAEMON_TEST) + runtime flag -d stubs ONLY the AO ioctl
 * layer (lines to a file given by env AO_DAEMON_TEST_LOG); FIFO/chunking/
 * amp-idle/signal logic is IDENTICAL. Env AO_DAEMON_TEST_FAIL_N fails the
 * Nth SendFrame once (recovery path). Env AO_DAEMON_FIFO overrides the
 * FIFO path (mock only).
 *
 * Flags: -v  per-frame logging (default: open/bind/ready/amp transitions,
 *             per-burst frame counts, errors only)
 *        -d  test-mode AO stub (only in a -DAO_DAEMON_TEST build)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FIFO_DEFAULT "/tmp/audio_in_fifo"
#define AO_DEV       "/dev/ao"
#define CPLD_DEV     "/dev/cpld_periph"

#define IOCTL_AO_BIND      0x40045800u  /* nr 0,  _IOW,  4 B */
#define IOCTL_AO_QUERY     0x8004581Bu  /* nr 27, _IOR,  4 B */
#define IOCTL_AO_SENDFRAME 0xC0785805u  /* nr 5,  _IOWR, 120 B */
#define IOCTL_CPLD_AMP_ON  0x7010u
#define IOCTL_CPLD_AMP_OFF 0x7011u

#define FRAME_BYTES   1280   /* 640 samples x 2 bytes, 40 ms @ 16 kHz */
#define ACCUM_SIZE    8192   /* accumulation buffer; drained after each read */
#define READ_CHUNK    4096   /* max bytes per read() */
#define AMP_IDLE_MS   2000   /* amp OFF after this long with no frame sent */
#define POLL_TIMEOUT  100    /* ms */

/* Playout (jitter) buffer — see header comment. */
#define PLAYOUT_DEPTH        10   /* prefill frames before starting (400 ms) */
#define PLAYOUT_TICK_MS      40   /* one frame released per 40 ms */
#define PLAYOUT_CAP          25   /* drop oldest beyond this depth */
#define PLAYOUT_QUEUE        32   /* ring slots (>= PLAYOUT_CAP + 1) */
#define PREFILL_TIMEOUT_MS   1000 /* start playout anyway after this long */
#define UNDERRUN_STOP_TICKS  50   /* stop playout after 2.0 s of underrun */

static const char *g_fifo_path = FIFO_DEFAULT;
static int g_verbose = 0;
static int g_test_mode = 0;

static int fd_fifo = -1;
static int fd_ao = -1;
static int fd_cpld = -1;
static int g_amp_on = 0;

static uint8_t g_accum[ACCUM_SIZE];
static size_t  g_accum_len = 0;
static uint8_t g_frame[FRAME_BYTES];

static unsigned long g_frames_sent = 0;   /* successful SendFrames */
static unsigned long g_burst_frames = 0;  /* frames in the current burst */
static long long g_last_frame_ms = -1;    /* monotonic ms of last sent frame */

/* Playout (jitter) buffer state. */
static uint8_t g_queue[PLAYOUT_QUEUE][FRAME_BYTES]; /* ring of frames */
static int g_q_head = 0, g_q_tail = 0, g_q_count = 0;
static int g_playing = 0;                /* playout ticker running */
static long long g_next_tick_ms = 0;     /* monotonic ms of next release */
static long long g_prefill_since_ms = -1; /* first frame of current prefill */
static int g_writer_gone = 0;            /* FIFO writer disconnected */
static unsigned long g_underruns = 0;    /* total silence frames sent */
static unsigned long g_underrun_runs = 0;
static unsigned long g_underrun_run_frames = 0; /* silence in current run */
static int g_in_underrun_run = 0;
static unsigned long g_drops = 0;        /* frames dropped at queue cap */
static const uint8_t g_silence[FRAME_BYTES] = {0};

static volatile sig_atomic_t g_stop = 0;

/* Playout functions (defined in the frame-pump section below; the FIFO
 * section calls playout_start on writer disconnect). */
static void playout_start(void);
static void playout_maybe_start(void);
static void playout_tick(void);

#ifdef AO_DAEMON_TEST
static FILE *g_test_log = NULL;   /* AO_DAEMON_TEST_LOG, or stderr if unset */
static unsigned long g_send_count = 0;  /* SendFrame calls (incl. failed) */
static long g_fail_n = 0;               /* AO_DAEMON_TEST_FAIL_N */
static int g_fail_done = 0;
#endif

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static long long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void log_msg(const char *fmt, ...)
{
    char tbuf[32];
    struct timespec ts;
    struct tm tm;
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s.%03ld] ", tbuf, (long)(ts.tv_nsec / 1000000));

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ------------------------------------------------------------------ */
/* signals                                                             */
/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ------------------------------------------------------------------ */
/* AO layer — real ioctls, or test-mode stubs (compile-time switch)    */
/* ------------------------------------------------------------------ */

#ifdef AO_DAEMON_TEST

static void test_log_line(const char *fmt, ...)
{
    FILE *f = g_test_log ? g_test_log : stderr;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

/* Stub bind+query: always ready. Logs BIND so the mock can see it. */
static int ao_bind(void)
{
    test_log_line("BIND ok (stub)");
    return 0;
}

/* Stub SendFrame: logs one line per call; fails the Nth call once. */
static int ao_send_frame(const uint8_t *frame, size_t len)
{
    g_send_count++;
    if (g_fail_n > 0 && (long)g_send_count == g_fail_n && !g_fail_done) {
        g_fail_done = 1;
        test_log_line("FRAME %lu FAILED (injected)", g_send_count);
        errno = EIO;
        return -1;
    }
    test_log_line("FRAME %lu %zu %02x%02x%02x%02x",
                  g_send_count, len, frame[0], frame[1], frame[2], frame[3]);
    return 0;
}

static int amp_on(void)
{
    test_log_line("AMP ON");
    g_amp_on = 1;
    return 0;
}

static int amp_off(void)
{
    test_log_line("AMP OFF");
    g_amp_on = 0;
    return 0;
}

static int cpld_open(void)
{
    /* No /dev/cpld_periph under qemu; stubbed away in test mode. */
    return 0;
}

#else /* real AO layer */

static int ao_bind(void)
{
    int id = 0;
    int r;

    errno = 0;
    r = ioctl(fd_ao, IOCTL_AO_BIND, &id);
    if (r < 0) {
        log_msg("[ao] ioctl nr 0 (bind id=0) FAILED ret=%d errno=%d (%s)",
                r, errno, strerror(errno));
        return -1;
    }
    log_msg("[ao] ioctl nr 0 (bind id=0) ok");

    /* Query ready: must be 1. Retry 500 ms up to ~30 s (boot race: rmm
     * may not have finished AO init yet), then every 5 s forever. Log
     * the first failure, a liveness line every ~30 s, and the success. */
    {
        int attempt = 0;
        for (;;) {
            int ready = -1;
            if (g_stop)
                return -1;  /* SIGTERM/SIGINT while waiting: exit promptly */
            errno = 0;
            r = ioctl(fd_ao, IOCTL_AO_QUERY, &ready);
            if (r >= 0 && ready == 1) {
                log_msg("[ao] ioctl nr 27 (query ready) -> ready=1");
                return 0;
            }
            if (attempt == 0) {
                log_msg("[ao] ioctl nr 27 not ready yet (ret=%d ready=%d "
                        "errno=%d) — retrying", r, ready,
                        r < 0 ? errno : 0);
            }
            attempt++;
            if (attempt % 60 == 0)
                log_msg("[ao] still waiting for nr 27 ready=1 "
                        "(attempt %d)", attempt);
            usleep(attempt < 60 ? 500000 : 5000000);
        }
    }
}

static int ao_send_frame(const uint8_t *frame, size_t len)
{
    uint8_t f120[120];
    int r;

    memset(f120, 0, sizeof(f120));
    *(uint32_t *)(f120 + 0)   = 1;    /* enBitwidth = 16-bit */
    *(uint32_t *)(f120 + 4)   = 0;    /* enSoundmode = mono */
    *(uint32_t *)(f120 + 8)   = (uint32_t)(uintptr_t)frame; /* pVirAddr[0] */
    *(uint32_t *)(f120 + 36)  = (uint32_t)len; /* u32Len */
    *(int32_t  *)(f120 + 112) = -1;   /* s32MilliSec = infinite timeout */

    errno = 0;
    r = ioctl(fd_ao, IOCTL_AO_SENDFRAME, f120);
    return r;
}

static int amp_on(void)
{
    int r;
    errno = 0;
    r = ioctl(fd_cpld, IOCTL_CPLD_AMP_ON, 0);
    if (r < 0) {
        log_msg("[amp] ioctl 0x7010 (amp ON) FAILED ret=%d errno=%d (%s)",
                r, errno, strerror(errno));
        return -1;
    }
    g_amp_on = 1;
    log_msg("[amp] ON");
    return 0;
}

static int amp_off(void)
{
    int r;
    errno = 0;
    r = ioctl(fd_cpld, IOCTL_CPLD_AMP_OFF, 0);
    if (r < 0) {
        log_msg("[amp] ioctl 0x7011 (amp OFF) FAILED ret=%d errno=%d (%s)",
                r, errno, strerror(errno));
        return -1;
    }
    g_amp_on = 0;
    log_msg("[amp] OFF");
    return 0;
}

static int cpld_open(void)
{
    fd_cpld = open(CPLD_DEV, O_RDWR);
    if (fd_cpld < 0) {
        log_msg("[FAIL] open(%s) -> errno=%d (%s)",
                CPLD_DEV, errno, strerror(errno));
        return -1;
    }
    log_msg("[ok] open(%s) -> fd=%d", CPLD_DEV, fd_cpld);
    return 0;
}

#endif /* AO_DAEMON_TEST */

/* Open /dev/ao (real) or stub-bind (test mode). Returns 0 on success. */
static int ao_open(void)
{
#ifdef AO_DAEMON_TEST
    (void)fd_ao;
    return ao_bind();
#else
    fd_ao = open(AO_DEV, O_RDWR);
    if (fd_ao < 0) {
        log_msg("[FAIL] open(%s) -> errno=%d (%s)",
                AO_DEV, errno, strerror(errno));
        return -1;
    }
    log_msg("[ok] open(%s) -> fd=%d", AO_DEV, fd_ao);
    if (ao_bind() < 0) {
        close(fd_ao);
        fd_ao = -1;
        return -1;
    }
    return 0;
#endif
}

static void ao_close(void)
{
#ifdef AO_DAEMON_TEST
    (void)0;
#else
    if (fd_ao >= 0) {
        close(fd_ao);
        fd_ao = -1;
    }
#endif
}

/* SendFrame failure recovery: close /dev/ao, re-open, re-bind, resume.
 * Never exits. Returns 0 on recovered, -1 if re-bind still failing
 * (caller keeps the frame dropped and continues; next frame retries). */
static int ao_recover(void)
{
    log_msg("[recovery] SendFrame failed — closing /dev/ao, re-binding");
#ifdef AO_DAEMON_TEST
    test_log_line("REBIND");
#endif
    ao_close();
    if (ao_open() < 0) {
        log_msg("[recovery] re-bind FAILED — will retry on next frame");
        return -1;
    }
    log_msg("[recovery] re-bound, resuming");
    return 0;
}

/* ------------------------------------------------------------------ */
/* FIFO                                                                */
/* ------------------------------------------------------------------ */

static int fifo_open(void)
{
    if (mkfifo(g_fifo_path, 0666) < 0 && errno != EEXIST) {
        log_msg("[FAIL] mkfifo(%s) -> errno=%d (%s)",
                g_fifo_path, errno, strerror(errno));
        return -1;
    }
    fd_fifo = open(g_fifo_path, O_RDONLY | O_NONBLOCK);
    if (fd_fifo < 0) {
        log_msg("[FAIL] open(%s) -> errno=%d (%s)",
                g_fifo_path, errno, strerror(errno));
        return -1;
    }
    log_msg("[ok] FIFO %s open for reading (fd=%d, held permanently)",
            g_fifo_path, fd_fifo);
    return 0;
}

static void fifo_reopen(void)
{
    log_msg("[fifo] writer disconnected (read==0) — re-opening read end");
    g_writer_gone = 1;
    if (g_accum_len > 0) {
        /* The writer's stream ended mid-frame: the leftover bytes are
         * the tail of the old call's last partial frame, not the head
         * of the next call's first frame (each call is a fresh PCM
         * stream from a new sink). Discard them, or the next call's
         * first frame would splice stale + new audio. */
        log_msg("[fifo] discarding %zu leftover bytes (< one frame) "
                "from the ended burst", g_accum_len);
        g_accum_len = 0;
    }
    if (fd_fifo >= 0) {
        close(fd_fifo);
        fd_fifo = -1;
    }
    /* Small pause so a no-writer poll()==POLLIN (EOF condition) cannot
     * spin the loop; the window is far below any audio timing need. */
    usleep(50000);
    if (fifo_open() < 0)
        log_msg("[fifo] re-open FAILED — retrying on next loop iteration");
    /* The queued tail of the ended call is still real audio: play it
     * out immediately instead of waiting for the prefill timeout. */
    if (!g_playing && g_q_count > 0)
        playout_start();
}

/* ------------------------------------------------------------------ */
/* frame pump                                                          */
/* ------------------------------------------------------------------ */

/* Send one complete 1280-byte frame. Handles amp-on at burst start and
 * SendFrame recovery. Returns 0 on sent, -1 on failure (frame dropped).
 * is_silence: the frame is a playout underrun filler (all zeros); it is
 * still a real SendFrame, but the verbose log line marks it so the
 * existing "[frame] #N sent" parsers keep counting audio frames only. */
static int send_frame_data(const uint8_t *data, int is_silence)
{
    int r;

    memcpy(g_frame, data, FRAME_BYTES);

    if (!g_amp_on) {
        if (amp_on() < 0) {
            /* Amp failure is not fatal to the frame itself; log and
             * continue — the speaker just stays quiet. */
            log_msg("[warn] amp ON failed, sending frame anyway");
        }
        g_burst_frames = 0;
    }

    r = ao_send_frame(g_frame, FRAME_BYTES);
    if (r < 0) {
        int e = errno;
        log_msg("[error] SendFrame FAILED ret=%d errno=%d (%s) "
                "— frame #%lu dropped, recovering",
                r, e, strerror(e), g_frames_sent + 1);
        ao_recover();
        return -1;
    }

    g_frames_sent++;
    g_burst_frames++;
    g_last_frame_ms = monotonic_ms();
    if (g_verbose) {
        if (is_silence)
            log_msg("[frame] #%lu silence sent (1280 B)", g_frames_sent);
        else
            log_msg("[frame] #%lu sent (1280 B)", g_frames_sent);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* playout (jitter) buffer                                             */
/* ------------------------------------------------------------------ */

/* Push one complete frame into the ring. Beyond PLAYOUT_CAP the oldest
 * frame is dropped (latency protection — the sender is running fast). */
static void queue_push(const uint8_t *frame)
{
    if (g_q_count >= PLAYOUT_CAP) {
        g_q_head = (g_q_head + 1) % PLAYOUT_QUEUE;
        g_q_count--;
        g_drops++;
        log_msg("[playout] queue full (%d frames) — dropped oldest frame "
                "(total drops %lu)", PLAYOUT_CAP, g_drops);
    }
    memcpy(g_queue[g_q_tail], frame, FRAME_BYTES);
    g_q_tail = (g_q_tail + 1) % PLAYOUT_QUEUE;
    g_q_count++;
    if (g_prefill_since_ms < 0)
        g_prefill_since_ms = monotonic_ms();
}

/* Pop the oldest queued frame. Returns 1 on success, 0 if empty. */
static int queue_pop(uint8_t *out)
{
    if (g_q_count <= 0)
        return 0;
    memcpy(out, g_queue[g_q_head], FRAME_BYTES);
    g_q_head = (g_q_head + 1) % PLAYOUT_QUEUE;
    g_q_count--;
    return 1;
}

/* Start the release ticker now (next tick immediately). */
static void playout_start(void)
{
    g_playing = 1;
    g_next_tick_ms = monotonic_ms();
    g_prefill_since_ms = -1;
    log_msg("[playout] started — depth %d frames (target %d)",
            g_q_count, PLAYOUT_DEPTH);
}

/* Start playout once the queue reached the prefill depth, or the
 * prefill timeout expired with at least one frame queued. */
static void playout_maybe_start(void)
{
    long long now;
    if (g_playing)
        return;
    if (g_q_count <= 0) {
        g_prefill_since_ms = -1;
        return;
    }
    now = monotonic_ms();
    if (g_q_count >= PLAYOUT_DEPTH ||
        (g_prefill_since_ms >= 0 &&
         now - g_prefill_since_ms >= PREFILL_TIMEOUT_MS))
        playout_start();
}

/* Release one frame at a tick: a real frame if queued, otherwise an
 * underrun (silence) or a stop (writer gone / long stall). */
static void release_one(void)
{
    if (g_q_count > 0) {
        uint8_t frame[FRAME_BYTES];
        if (!queue_pop(frame))
            return;
        if (g_in_underrun_run) {
            log_msg("[underrun] run #%lu ended: %lu silence frames sent",
                    g_underrun_runs, g_underrun_run_frames);
            g_in_underrun_run = 0;
            g_underrun_run_frames = 0;
        }
        send_frame_data(frame, 0);
        return;
    }

    /* Queue empty at a release tick. */
    if (g_writer_gone) {
        if (g_in_underrun_run) {
            log_msg("[underrun] run #%lu ended: %lu silence frames sent",
                    g_underrun_runs, g_underrun_run_frames);
            g_in_underrun_run = 0;
            g_underrun_run_frames = 0;
        }
        log_msg("[playout] stopped — queue drained after writer disconnect");
        g_playing = 0;
        return;
    }

    /* Mid-call stall: log the run start once (default level, with
     * timestamp), then send one silence frame per tick so the AO buffer
     * stays fed. A run of UNDERRUN_STOP_TICKS is a pause, not jitter —
     * stop playout and let the amp idle off as usual. */
    if (!g_in_underrun_run) {
        g_underrun_runs++;
        g_in_underrun_run = 1;
        g_underrun_run_frames = 0;
        log_msg("[underrun] run #%lu started — queue empty at release "
                "tick, sending silence", g_underrun_runs);
    }
    g_underrun_run_frames++;
    g_underruns++;
    send_frame_data(g_silence, 1);
    if (g_underrun_run_frames >= UNDERRUN_STOP_TICKS) {
        log_msg("[underrun] run #%lu ended after %lu silence frames "
                "(%.1f s gap) — stopping playout",
                g_underrun_runs, g_underrun_run_frames,
                g_underrun_run_frames * (PLAYOUT_TICK_MS / 1000.0));
        g_in_underrun_run = 0;
        g_underrun_run_frames = 0;
        g_playing = 0;
    }
}

/* Release due frames on the monotonic 40 ms schedule. If we fell far
 * behind (e.g. blocked in SendFrame), resync instead of dumping a burst
 * of catch-up frames. At most 3 releases per call. */
static void playout_tick(void)
{
    long long now;
    int n = 0;
    if (!g_playing)
        return;
    now = monotonic_ms();
    if (now - g_next_tick_ms > 3 * PLAYOUT_TICK_MS)
        g_next_tick_ms = now;
    while (g_playing && now >= g_next_tick_ms && n < 3) {
        release_one();
        g_next_tick_ms += PLAYOUT_TICK_MS;
        n++;
        now = monotonic_ms();
    }
}

/* Drain complete frames from the accumulation buffer into the playout
 * queue (they are released by the ticker, not sent immediately). */
static void drain_frames(void)
{
    while (g_accum_len >= FRAME_BYTES) {
        queue_push(g_accum);
        g_accum_len -= FRAME_BYTES;
        if (g_accum_len > 0)
            memmove(g_accum, g_accum + FRAME_BYTES, g_accum_len);
    }
}

/* Amp idle check: OFF after AMP_IDLE_MS with no frame sent. */
static void amp_idle_check(void)
{
    if (g_amp_on && g_last_frame_ms >= 0 &&
        monotonic_ms() - g_last_frame_ms >= AMP_IDLE_MS) {
        unsigned long n = g_burst_frames;
        amp_off();
        log_msg("[burst] complete: %lu frames sent", n);
        g_burst_frames = 0;
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-v] [-d]\n"
        "  -v  per-frame logging\n"
        "  -d  test-mode AO stub (only in -DAO_DAEMON_TEST build)\n"
        "  playout buffer: %d-frame prefill, 1 frame/%d ms, queue cap %d\n"
        "env: AO_DAEMON_FIFO (default /tmp/audio_in_fifo)\n"
        "     AO_DAEMON_TEST_LOG, AO_DAEMON_TEST_FAIL_N (test build only)\n",
        prog, PLAYOUT_DEPTH, PLAYOUT_TICK_MS, PLAYOUT_CAP);
}

int main(int argc, char *argv[])
{
    int opt;
    struct sigaction sa;

    while ((opt = getopt(argc, argv, "vd")) != -1) {
        switch (opt) {
        case 'v':
            g_verbose = 1;
            break;
        case 'd':
            g_test_mode = 1;
            break;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    if (optind < argc) {
        usage(argv[0]);
        return 2;
    }

    {
        const char *f = getenv("AO_DAEMON_FIFO");
        if (f && *f)
            g_fifo_path = f;
    }

#ifdef AO_DAEMON_TEST
    if (g_test_mode) {
        const char *logpath = getenv("AO_DAEMON_TEST_LOG");
        const char *failn = getenv("AO_DAEMON_TEST_FAIL_N");
        if (failn)
            g_fail_n = atol(failn);
        if (logpath && *logpath) {
            g_test_log = fopen(logpath, "a");
            if (!g_test_log) {
                fprintf(stderr, "[ao_daemon] cannot open AO_DAEMON_TEST_LOG=%s: %s\n",
                        logpath, strerror(errno));
                return 1;
            }
        }
        log_msg("[ao_daemon] TEST MODE: AO ioctls stubbed "
                "(log=%s, fail_n=%ld)",
                logpath ? logpath : "(stderr)", g_fail_n);
    }
#else
    if (g_test_mode) {
        fprintf(stderr, "[ao_daemon] -d given but this binary was not "
                        "built with -DAO_DAEMON_TEST\n");
        return 2;
    }
#endif

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    log_msg("[ao_daemon] start — FIFO %s, frame %d B, amp idle %d ms, "
            "playout depth %d, tick %d ms, queue cap %d",
            g_fifo_path, FRAME_BYTES, AMP_IDLE_MS,
            PLAYOUT_DEPTH, PLAYOUT_TICK_MS, PLAYOUT_CAP);

    if (fifo_open() < 0)
        return 1;

    if (ao_open() < 0) {
        /* Real mode: /dev/ao missing (e.g. rmm not up yet) — ao_bind's
         * retry loop covers "not ready", but a failed open is fatal at
         * startup; the watchdog (system.sh) restarts us. A SIGTERM
         * that arrived while waiting for nr 27 ready exits 0. */
        log_msg("[FAIL] AO open/bind failed at startup — exiting");
        close(fd_fifo);
        return g_stop ? 0 : 1;
    }

    if (cpld_open() < 0) {
        log_msg("[FAIL] CPLD open failed at startup — exiting");
        ao_close();
        close(fd_fifo);
        return 1;
    }

    log_msg("[ao_daemon] running — waiting for PCM on FIFO");

    while (!g_stop) {
        struct pollfd pfd;
        int pr;

        if (fd_fifo < 0) {
            /* fifo_reopen() failed earlier: retry ~once per second.
             * (Without this, poll() on fd -1 would just time out
             * forever and the daemon would stay deaf.) */
            static long long last_retry_ms = 0;
            long long now = monotonic_ms();
            if (now - last_retry_ms >= 1000) {
                last_retry_ms = now;
                if (fifo_open() < 0) {
                    playout_maybe_start();
                    playout_tick();
                    amp_idle_check();
                    usleep(100000);
                    continue;
                }
            } else {
                playout_maybe_start();
                playout_tick();
                amp_idle_check();
                usleep(100000);
                continue;
            }
        }

        pfd.fd = fd_fifo;
        pfd.events = POLLIN;
        pfd.revents = 0;

        /* When playout is running, wake at the next release tick so the
         * 40 ms cadence does not depend on FIFO activity. */
        {
            int poll_timeout = POLL_TIMEOUT;
            if (g_playing) {
                long long dt = g_next_tick_ms - monotonic_ms();
                if (dt < 0) dt = 0;
                if (dt > POLL_TIMEOUT) dt = POLL_TIMEOUT;
                poll_timeout = (int)dt;
            }
            pr = poll(&pfd, 1, poll_timeout);
        }
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            log_msg("[error] poll failed errno=%d (%s)", errno, strerror(errno));
            continue;
        }

        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            size_t room = ACCUM_SIZE - g_accum_len;
            size_t want = room < READ_CHUNK ? room : READ_CHUNK;
            ssize_t n = 0;

            if (want > 0)
                n = read(fd_fifo, g_accum + g_accum_len, want);

            if (n > 0) {
                g_writer_gone = 0;
                g_accum_len += (size_t)n;
                drain_frames();
            } else if (n == 0) {
                /* All writers gone — re-open, keep running. */
                fifo_reopen();
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                log_msg("[error] read failed errno=%d (%s)",
                        errno, strerror(errno));
            }
        }

        playout_maybe_start();
        playout_tick();
        amp_idle_check();
    }

    /* Shutdown: amp OFF first (no hiss / powered amp left behind),
     * then close fds. Plain kill (SIGTERM) lands here. Log the burst
     * count even when the amp is still on at shutdown. */
    if (g_amp_on) {
        unsigned long n = g_burst_frames;
        amp_off();
        log_msg("[burst] complete: %lu frames sent", n);
        g_burst_frames = 0;
    } else if (g_burst_frames > 0) {
        log_msg("[burst] complete: %lu frames sent", g_burst_frames);
        g_burst_frames = 0;
    }

    log_msg("[ao_daemon] shutdown — %lu frames sent total, exiting",
            g_frames_sent);

    if (fd_fifo >= 0)
        close(fd_fifo);
    ao_close();
    if (fd_cpld >= 0)
        close(fd_cpld);
#ifdef AO_DAEMON_TEST
    if (g_test_log)
        fclose(g_test_log);
#endif
    return 0;
}
