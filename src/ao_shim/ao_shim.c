/*
 * ao_shim.c — LD_PRELOAD audio-output shim for the Yi 1080p Home camera.
 *
 * LOCAL BUILD ONLY. Nothing in this file talks to a camera; it is a
 * shared object to be LD_PRELOADed into the camera's stock ./rmm process
 * at a later, separately-approved deployment step.
 *
 * ----------------------------------------------------------------
 * Protocol (PROVEN by the controlled tone test on the live camera,
 * 2026-11-16 23:46 WIB — 25/25 nr-5 frames accepted, 1 kHz tone heard;
 * see ao_tone_test.c for the full record):
 *
 *   The stock rmm drives the speaker ONLY via ioctl() on /dev/ao.
 *   There is no ALSA, no FIFO, no write(). The audio ioctl is:
 *
 *     nr 5  (0xC0785805, _IOWR, 120 B) : SendFrame — the ONLY
 *             audio-carrying ioctl. 16 kHz, 16-bit, mono, 640 samples
 *             (1280 bytes) per frame.
 *
 *   120-byte struct, zeroed, only these fields filled:
 *     +0   enBitwidth  = 1 (16-bit, u32)
 *     +4   enSoundmode = 0 (mono, u32)
 *     +8   pVirAddr[0] = pointer to the 1280-byte PCM chunk (u32)
 *     +36  u32Len      = 1280 (u32)
 *     +112 s32MilliSec = -1 (0xffffffff, infinite timeout, i32)
 *   All other fields 0, u32PhyAddr[0] = 0. The driver reads the PCM
 *   through pVirAddr, so a plain userspace buffer works.
 *
 *   Other AO ioctls (observed from rmm, never issued by this shim):
 *     nr 0  (0x40045800) : bind fd to AO channel 0
 *     nr 1  (0x40245801) : SetPubAttr — channel configured
 *     nr 6  (0x5806)     : EnableChn — channel enabled
 *     nr 27 (0x8004581B) : query channel ready, must return 1
 *
 *   Amplifier, via /dev/cpld_periph:
 *     ioctl 0x7010 arg 0 = amp ON, ioctl 0x7011 arg 0 = amp OFF.
 *     The amp is OFF when idle and must never be left on indefinitely.
 *
 * ----------------------------------------------------------------
 * Design:
 *
 *   * Intercepts the PLT calls open/open64/openat/ioctl/close.
 *     Forwarding is done with RAW syscalls (svc), never by calling
 *     the intercepted names again — so there is no recursion and no
 *     need for dlsym(RTLD_NEXT) (contrast ipc_multiplex.c, which
 *     uses dlsym and therefore needs libdl).
 *   * Tracks the /dev/ao fd (seen via open) and the /dev/cpld_periph
 *     fd. If rmm never opens the CPLD device, the shim opens it
 *     itself at activation time — rmm has no reason to touch the amp.
 *   * Activates ONLY once rmm has enabled an AO channel, observed as
 *     a successful nr 1 (SetPubAttr 0x40245801) or nr 6 (EnableChn
 *     0x5806) ioctl on the ao fd. The shim NEVER configures the AO
 *     device itself: no SetPubAttr / Enable / EnableChn from us.
 *   * On activation: mkfifo /tmp/audio_in_fifo (if absent) and spawn
 *     one reader thread. The thread reads 16 kHz mono S16LE PCM from
 *     the FIFO in 640-sample (1280-byte) chunks, builds the 120-byte
 *     nr-5 struct, and issues the real nr-5 ioctl on the ao fd.
 *     FIFO EOF (writer closed) -> close and reopen; short reads are
 *     accumulated to full 1280-byte chunks; a partial tail survives
 *     across reopen (a burst is a byte stream, not messages).
 *   * Amp: turned ON (0x7010) before the first frame of a burst,
 *     turned OFF (0x7011) after ~2 s with no FIFO data. Never left
 *     on indefinitely.
 *   * rmm's own ioctls are forwarded FIRST and promptly; the shim's
 *     extra work (activation) happens after the forward returns.
 *
 * ----------------------------------------------------------------
 * Toolchain constraint (why this file looks the way it does):
 *
 *   The hisiv300 toolchain is NOT obtainable, so this is built with
 *   Debian arm-linux-gnueabi-gcc. The camera runs uClibc, not glibc,
 *   so a .so linked against glibc would NOT load under the camera's
 *   uClibc loader. Therefore this file is built with
 *   -nostdlib -shared -fPIC and uses ONLY raw syscalls internally:
 *   no libc calls, no pthread (the reader thread is spawned with the
 *   raw clone() syscall and a mmap'd stack), no malloc (static BSS
 *   buffers only), no division/modulo and no 64-bit arithmetic (that
 *   would pull in __aeabi_* libcalls, which do not exist without a
 *   libc). Verify with readelf: the .so must have NO NEEDED entries
 *   for libc.so.6 / libdl / libpthread, and `nm -u` must show exactly
 *   ONE undefined symbol: `__errno_location` (used only to set errno
 *   on failure; it resolves from the already-loaded uClibc, so no
 *   DT_NEEDED is needed).
 *
 *   ARM EABI syscall numbers used:
 *     exit 1, read 3, write 4, open 5, close 6, time 13, mknod 14,
 *     ioctl 54, clone 120, nanosleep 162, mmap2 192, openat 322.
 *
 * ----------------------------------------------------------------
 * Crash-safety (this runs inside rmm — a crash kills the camera):
 *
 *   * Every syscall return is checked; every pointer is NULL-checked
 *     before use; no abort()/exit() of the host process, ever — the
 *     only exit() call is in the cloned reader thread, which exits
 *     only itself.
 *   * The reader thread tolerates ioctl errors (counts them, keeps
 *     going) and a vanishing ao fd (stops injecting, turns the amp
 *     off, waits for the ao fd to come back).
 *   * No locks: state is plain volatile ints/flags written
 *     idempotently; the reader only reads g_ao_fd, which is set
 *     before the thread is spawned (clone is the barrier).
 *
 * ----------------------------------------------------------------
 * Known limitations (honest list — see README.md):
 *
 *   * errno IS set on failure (H1 fix, 2026-11-17). Intercepted
 *     open/ioctl/close call __errno_location() (exported by the
 *     camera's uClibc, verified on libuClibc-0.9.33.2.so) and store
 *     the failed syscall's -errno there, so callers see the real
 *     errno. Stale errno was suspected to kill rmm in early startup,
 *     but foreground test 2 with this fix changed nothing, so H1 was
 *     refuted (2026-11-17). The old README claim that errno could
 *     not be accessed from this .so was wrong.
 *   * Only open/open64/openat are intercepted. If rmm ever opened
 *     /dev/ao via another spelling (e.g. __open_2, or a pre-opened
 *     fd inherited across exec), the ao fd would not be tracked and
 *     the shim would stay inert — safe, but silent.
 *   * Activation keys off nr 1 / nr 6 succeeding on the ao fd. If a
 *     future rmm build enables the channel another way, the shim
 *     will not activate.
 *   * A burst must deliver complete 1280-byte chunks eventually; a
 *     partial tail is held (not discarded) until more data arrives.
 *   * AO_SHIM_TEST_HOOK (Makefile test build only, never in the
 *     shipped .so) additionally treats TCGETS (0x5401) on the ao fd
 *     as an activation trigger, so the reader path can be exercised
 *     under qemu-arm against a pty standing in for /dev/ao.
 */

/* No #includes: -nostdlib, freestanding, raw syscalls only. */

/* ---- ARM EABI syscall numbers ---- */
#define SYS_EXIT      1
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_TIME      13
#define SYS_MKNOD     14
#define SYS_IOCTL     54
#define SYS_CLONE     120
#define SYS_NANOSLEEP 162
#define SYS_MMAP2     192
#define SYS_OPENAT    322

/* ---- Device / protocol constants (from the proven tone test) ---- */
#define AO_DEV        "/dev/ao"
#define CPLD_DEV      "/dev/cpld_periph"
#define FIFO_PATH     "/tmp/audio_in_fifo"
#define DEBUG_FLAG    "/tmp/ao_shim_debug"

#define AO_SETPUBATTR 0x40245801UL /* nr 1  */
#define AO_ENABLECHN  0x5806UL     /* nr 6  */
#define AO_SENDFRAME  0xC0785805UL /* nr 5, 120 B */
#define CPLD_AMP_ON   0x7010UL
#define CPLD_AMP_OFF  0x7011UL

#define FRAME_BYTES   1280         /* 640 samples x 2 bytes */
#define AMP_IDLE_SEC  2            /* amp off after ~2 s with no data */
#define STACK_BYTES   65536

/* open flags */
#define O_RDONLY      0
#define O_RDWR        2
#define O_NONBLOCK    0x800        /* 04000 octal */

/* mmap */
#define PROT_READ     1
#define PROT_WRITE    2
#define MAP_PRIVATE   2
#define MAP_ANONYMOUS 0x20

/* clone flags: VM|FS|FILES|SIGHAND|THREAD|SYSVSEM */
#define CLONE_FLAGS   0x50F00

/* mknod mode: S_IFIFO (010000 octal) | 0666 */
#define FIFO_MODE     0x11B6

/* ---------------------------------------------------------------- */
/* Raw syscall plumbing                                              */
/* ---------------------------------------------------------------- */

/* One generic 6-arg raw syscall. Args go in r0-r5, number in r7. */
static long raw_sys(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    register long r0 __asm__("r0") = a1;
    register long r1 __asm__("r1") = a2;
    register long r2 __asm__("r2") = a3;
    register long r3 __asm__("r3") = a4;
    register long r4 __asm__("r4") = a5;
    register long r5 __asm__("r5") = a6;
    register long r7 __asm__("r7") = n;
    __asm__ volatile ("svc #0"
        : "+r" (r0)
        : "r" (r1), "r" (r2), "r" (r3), "r" (r4), "r" (r5), "r" (r7)
        : "memory");
    return r0; /* negative => -errno */
}

static long sys_exit(long code)   { return raw_sys(SYS_EXIT, code, 0, 0, 0, 0, 0); }
static long sys_read(int fd, long buf, long len)
{ return raw_sys(SYS_READ, fd, buf, len, 0, 0, 0); }
static long sys_write(int fd, long buf, long len)
{ return raw_sys(SYS_WRITE, fd, buf, len, 0, 0, 0); }
static long sys_open(const char *path, long flags, long mode)
{ return raw_sys(SYS_OPEN, (long)path, flags, mode, 0, 0, 0); }
static long sys_close(int fd)     { return raw_sys(SYS_CLOSE, fd, 0, 0, 0, 0, 0); }
static long sys_time(void)        { return raw_sys(SYS_TIME, 0, 0, 0, 0, 0, 0); }
static long sys_mknod(const char *path, long mode, long dev)
{ return raw_sys(SYS_MKNOD, (long)path, mode, dev, 0, 0, 0); }
static long sys_ioctl(int fd, unsigned long req, long arg)
{ return raw_sys(SYS_IOCTL, fd, (long)req, arg, 0, 0, 0); }
static long sys_clone(long flags, long stack, long ptid, long tls, long ctid)
{ return raw_sys(SYS_CLONE, flags, stack, ptid, tls, ctid, 0); }
static long sys_mmap2(long addr, long len, long prot, long flags, long fd, long pgoff)
{ return raw_sys(SYS_MMAP2, addr, len, prot, flags, fd, pgoff); }

static void sleep_ms(long ms)
{
    long ts[2];
    ts[0] = 0;
    ts[1] = ms * 1000000L; /* ms <= 200 here, no overflow */
    raw_sys(SYS_NANOSLEEP, (long)ts, 0, 0, 0, 0, 0);
}

/* ---------------------------------------------------------------- */
/* Tiny string helpers (no libc)                                     */
/* ---------------------------------------------------------------- */

static long s_len(const char *s)
{
    long n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int s_eq(const char *a, const char *b)
{
    long i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

/* ---------------------------------------------------------------- */
/* State (BSS + explicit lazy init; init is idempotent, so a race    */
/* between two rmm threads calling ensure_init() is benign)          */
/* ---------------------------------------------------------------- */

static volatile int  g_ready;
static volatile int  g_ao_fd;
static volatile int  g_cpld_fd;
static volatile int  g_cpld_ours;      /* 1 if the shim opened the cpld fd */
static volatile int  g_cpld_gave_up;   /* cpld open failed; don't spam it */
static volatile int  g_activated;
static volatile int  g_thread_running;
static volatile int  g_amp_on;
static volatile int  g_debug;
static volatile unsigned long g_frames_sent;
static volatile unsigned long g_ioctl_errors;
static volatile int  g_frame_built;

static unsigned char g_pcm[FRAME_BYTES]; /* 1280-byte PCM chunk buffer */
static unsigned char g_frame[120];       /* 120-byte nr-5 struct */

static void ensure_init(void)
{
    if (g_ready) return;
    g_ao_fd = -1;
    g_cpld_fd = -1;
    g_cpld_ours = 0;
    g_cpld_gave_up = 0;
    g_activated = 0;
    g_thread_running = 0;
    g_amp_on = 0;
    g_debug = 0;
    g_frames_sent = 0;
    g_ioctl_errors = 0;
    g_frame_built = 0;
    g_ready = 1;
}

/* ---------------------------------------------------------------- */
/* Debug logging (fd 2, only if /tmp/ao_shim_debug exists at         */
/* activation time). Decimal printing uses subtraction only — no     */
/* division, so no __aeabi_* libcall is ever emitted.                */
/* ---------------------------------------------------------------- */

static const unsigned long dbg_pow10[10] =
    { 1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
      10000UL, 1000UL, 100UL, 10UL, 1UL };

static void dbg_u32(unsigned long v)
{
    char buf[12];
    int i = 0, k, started = 0;
    if (!g_debug) return;
    for (k = 0; k < 10; k++) {
        unsigned long p = dbg_pow10[k];
        unsigned int d = 0;
        while (v >= p) { v -= p; d++; }
        if (started || d != 0 || k == 9) { buf[i++] = (char)('0' + d); started = 1; }
    }
    sys_write(2, (long)buf, i);
}

static void dbg_msg(const char *s)
{
    if (!g_debug || !s) return;
    sys_write(2, (long)s, s_len(s));
}

static void dbg_msg_num(const char *s, unsigned long v)
{
    if (!g_debug) return;
    dbg_msg(s);
    dbg_u32(v);
    dbg_msg("\n");
}

/* ---------------------------------------------------------------- */
/* nr-5 frame construction                                           */
/* ---------------------------------------------------------------- */

static void put32(unsigned off, unsigned long v)
{
    g_frame[off]     = (unsigned char)(v & 0xFF);
    g_frame[off + 1] = (unsigned char)((v >> 8) & 0xFF);
    g_frame[off + 2] = (unsigned char)((v >> 16) & 0xFF);
    g_frame[off + 3] = (unsigned char)((v >> 24) & 0xFF);
}

static void build_frame(void)
{
    int i;
    for (i = 0; i < 120; i++) g_frame[i] = 0;
    put32(0, 1);                          /* enBitwidth = 16-bit */
    put32(4, 0);                          /* enSoundmode = mono */
    put32(8, (unsigned long)g_pcm);       /* pVirAddr[0] */
    put32(36, FRAME_BYTES);               /* u32Len = 1280 */
    put32(112, 0xFFFFFFFFUL);             /* s32MilliSec = -1 */
    g_frame_built = 1;
}

/* ---------------------------------------------------------------- */
/* Amp control                                                       */
/* ---------------------------------------------------------------- */

static void amp_on(void)
{
    long r;
    if (g_amp_on) return;
    if (g_cpld_fd < 0) {
        long c;
        if (g_cpld_gave_up) return;
        c = sys_open(CPLD_DEV, O_RDWR, 0);
        if (c < 0) {
            g_cpld_gave_up = 1;
            dbg_msg("[ao_shim] cpld open failed, sending frames without amp\n");
            return;
        }
        g_cpld_fd = (int)c;
        g_cpld_ours = 1;
    }
    r = sys_ioctl(g_cpld_fd, CPLD_AMP_ON, 0);
    if (r >= 0) {
        g_amp_on = 1;
        dbg_msg("[ao_shim] amp ON\n");
    } else {
        dbg_msg_num("[ao_shim] amp ON ioctl failed errno=", (unsigned long)(-r));
    }
}

static void amp_off(void)
{
    long r;
    if (!g_amp_on) return;
    if (g_cpld_fd >= 0) {
        r = sys_ioctl(g_cpld_fd, CPLD_AMP_OFF, 0);
        if (r < 0)
            dbg_msg_num("[ao_shim] amp OFF ioctl failed errno=", (unsigned long)(-r));
    }
    g_amp_on = 0;
    dbg_msg("[ao_shim] amp OFF\n");
}

/* ---------------------------------------------------------------- */
/* Frame injection                                                   */
/* ---------------------------------------------------------------- */

static void send_frame(void)
{
    int ao;
    long r;
    if (!g_frame_built) build_frame();
    ao = g_ao_fd;
    if (ao < 0) return;
    amp_on();
    ao = g_ao_fd; /* re-read: amp_on never touches it, but be safe */
    if (ao < 0) return;
    r = sys_ioctl(ao, AO_SENDFRAME, (long)g_frame);
    if (r < 0) {
        g_ioctl_errors++;
        if (g_ioctl_errors <= 3)
            dbg_msg_num("[ao_shim] nr5 ioctl failed errno=", (unsigned long)(-r));
    } else {
        g_frames_sent++;
        if (g_frames_sent <= 3)
            dbg_msg_num("[ao_shim] nr5 frame sent n=", g_frames_sent);
    }
}

/* ---------------------------------------------------------------- */
/* Reader thread: FIFO -> nr-5 ioctls. Runs forever; exits only      */
/* itself (sys_exit on a CLONE_THREAD child ends just that thread).   */
/* ---------------------------------------------------------------- */

static void reader_main(void)
{
    unsigned int have = 0; /* bytes accumulated in g_pcm */
    long last_data_sec = sys_time();

    dbg_msg("[ao_shim] reader thread started\n");
    for (;;) {
        long ffd;
        int done;

        if (g_ao_fd < 0) { sleep_ms(100); continue; }

        ffd = sys_open(FIFO_PATH, O_RDONLY | O_NONBLOCK, 0);
        if (ffd < 0) { sleep_ms(200); continue; }

        done = 0;
        while (!done) {
            int ao = g_ao_fd;
            long n;
            if (ao < 0) { done = 1; break; }
            n = sys_read((int)ffd, (long)(g_pcm + have), FRAME_BYTES - have);
            if (n > 0) {
                have += (unsigned int)n;
                last_data_sec = sys_time();
                if (have >= FRAME_BYTES) { send_frame(); have = 0; }
            } else if (n == 0) {
                done = 1; /* writer closed -> reopen */
            } else {
                /* n < 0: -EAGAIN when empty (normal), anything else
                 * is also tolerated: wait a little and retry. */
                long now = sys_time();
                if (g_amp_on && (now - last_data_sec) >= AMP_IDLE_SEC)
                    amp_off();
                sleep_ms(20);
            }
        }
        sys_close((int)ffd);
        if (g_amp_on) amp_off();
        /* loop: reopen the FIFO (a writer closing/reopening is normal) */
    }
}

/* ---------------------------------------------------------------- */
/* Activation: only after rmm itself enabled the AO channel.         */
/* ---------------------------------------------------------------- */

static void maybe_activate(void)
{
    long dfd, r, stack, top, tid;

    if (g_activated) return;
    g_activated = 1;

    dfd = sys_open(DEBUG_FLAG, O_RDONLY, 0);
    if (dfd >= 0) { g_debug = 1; sys_close((int)dfd); }

    dbg_msg("[ao_shim] activated: AO channel enabled by rmm\n");

    r = sys_mknod(FIFO_PATH, FIFO_MODE, 0);
    if (r < 0 && r != -17) /* -EEXIST: already there, fine */
        dbg_msg_num("[ao_shim] mkfifo failed errno=", (unsigned long)(-r));
    else
        dbg_msg("[ao_shim] fifo ready /tmp/audio_in_fifo\n");

    build_frame();

    if (g_cpld_fd < 0) {
        long c = sys_open(CPLD_DEV, O_RDWR, 0);
        if (c >= 0) {
            g_cpld_fd = (int)c;
            g_cpld_ours = 1;
            dbg_msg("[ao_shim] cpld opened by shim\n");
        } else {
            dbg_msg("[ao_shim] cpld not opened (will retry on first frame)\n");
        }
    }

    stack = sys_mmap2(0, STACK_BYTES, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack <= 0) {
        dbg_msg_num("[ao_shim] mmap stack failed errno=", (unsigned long)(-stack));
        return; /* inert but harmless: rmm keeps working */
    }
    top = (stack + STACK_BYTES - 16) & ~7L;

    tid = sys_clone(CLONE_FLAGS, top, 0, 0, 0);
    if (tid == 0) {
        reader_main(); /* never returns */
        sys_exit(0);   /* exits only this thread (CLONE_THREAD) */
    } else if (tid < 0) {
        dbg_msg_num("[ao_shim] clone failed errno=", (unsigned long)(-tid));
    } else {
        g_thread_running = 1;
        dbg_msg_num("[ao_shim] reader thread spawned tid=", (unsigned long)tid);
    }
}

/* ---------------------------------------------------------------- */
/* errno on failure (H1 fix, 2026-11-17).                           */
/*                                                                    */
/* The camera's uClibc exports __errno_location (verified on          */
/* libuClibc-0.9.33.2.so from the yi-hack-v5 package: FUNC WEAK      */
/* DEFAULT 0x15ae4; errno itself is a TLS symbol, so it must be      */
/* reached through this function, never a fixed address). It          */
/* resolves from the already-loaded libc — no DT_NEEDED needed,      */
/* exactly one undefined symbol in the .so. Before this fix the      */
/* shim returned -1 on failure but left errno stale, which killed    */
/* rmm in early startup (foreground test 1, ~5.6 s, exit -1,         */
/* 0-byte log). Callers that only check ret < 0 are unaffected       */
/* either way; callers that read errno now see the real value.       */
/* ---------------------------------------------------------------- */

extern int *__errno_location(void);

static void set_errno(long neg_errno)
{
    int *e = __errno_location();
    if (e) *e = (int)(-neg_errno);
}

/* ---------------------------------------------------------------- */
/* Call logging (diagnostic instrument, 2026-11-17).                 */
/*                                                                    */
/* Records EVERY intercepted call to /tmp/ao_shim_calls.log so a      */
/* foreground test can show the exact call that kills rmm in early   */
/* startup: the LAST line before death is the prime suspect.          */
/*                                                                    */
/* Rules (these are what make the instrument trustworthy):            */
/*   * Raw syscalls only: the log is opened with sys_open and each    */
/*     entry is a SINGLE sys_write. No stdio, no malloc, no call     */
/*     to any intercepted function from the logging path — so the     */
/*     logging path can never recurse into itself (a g_log_busy      */
/*     guard is kept anyway as a second line of defence).             */
/*   * The log is opened ONCE, lazily, on the first intercepted       */
/*     call (O_CREAT|O_APPEND|O_WRONLY, 0644). A logging failure      */
/*     (open/write fails) is silently ignored: logging NEVER          */
/*     changes a return value and NEVER crashes rmm. A failed write   */
/*     marks the log dead, and nothing is logged after that.          */
/*   * Entries are capped (4096): /tmp is RAM and small on the        */
/*     camera. No locks: under thread contention an entry may be      */
/*     dropped (g_log_busy), never corrupted.                         */
/*   * errno on a line is read from __errno_location() AFTER the      */
/*     call (and after set_errno on failure), i.e. exactly what the   */
/*     caller sees. On a SUCCESS line the value is whatever stale     */
/*     value the process had — ignore it; only failure errno values   */
/*     are meaningful. Paths are truncated to 125 chars + "...".      */
/*     Buffer contents are NEVER logged.                              */
/*   * Known limitation: the log fd is an ordinary fd of the host     */
/*     process. If rmm ever closed it blindly (e.g. a close-all-fds   */
/*     sweep at startup), the close() wrapper records a               */
/*     "!! log fd N closed by host" marker line BEFORE forwarding     */
/*     the close, then stops logging — the log ends with the marker   */
/*     instead of silently. (A close of the log fd cannot come from   */
/*     rmm's own code: rmm never learns the fd number, and the        */
/*     kernel never hands an in-use fd number to another open.)       */
/*                                                                    */
/* Line format (one line per call, sequence number first):            */
/*   [000123] open("/dev/ao", flags=0x00000002, mode=0000) = 3 errno=0 */
/*   [000124] ioctl(3, req=0x40245801, arg=0xb6f7e000) = 0 errno=0     */
/*   [000125] open("/no/such/file", flags=0x00000000, mode=0000) = -1 errno=2 */
/* ---------------------------------------------------------------- */

#define LOG_PATH       "/tmp/ao_shim_calls.log"
#define LOG_OPEN_FLAGS 0x441 /* O_WRONLY(1) | O_CREAT(0100) | O_APPEND(02000) */
#define LOG_MODE       0644
#define LOG_CAP        4096
#define LOG_PATH_CHARS 125
#define LOGBUF_LEN     320

static volatile int g_log_state; /* 0 = never tried, 1 = open, 2 = dead */
static volatile int g_log_fd;
static volatile int g_log_count;
static volatile int g_log_busy;

static char g_logbuf[LOGBUF_LEN];
static long g_loglen;

static void lb_reset(void) { g_loglen = 0; }

static void lb_ch(char c)
{
    if (g_loglen < LOGBUF_LEN - 1) g_logbuf[g_loglen++] = c;
}

static void lb_str(const char *s)
{
    long i = 0;
    if (!s) return;
    while (s[i]) { lb_ch(s[i]); i++; }
}

/* Decimal, subtraction only (no division: that would pull in
 * __aeabi_* libcalls). Reuses the dbg_pow10 table. */
static void lb_u32(unsigned long v)
{
    int k, started = 0;
    for (k = 0; k < 10; k++) {
        unsigned long p = dbg_pow10[k];
        unsigned int d = 0;
        while (v >= p) { v -= p; d++; }
        if (started || d != 0 || k == 9) { lb_ch((char)('0' + d)); started = 1; }
    }
}

static void lb_i32(long v)
{
    if (v < 0) { lb_ch('-'); lb_u32(0UL - (unsigned long)v); }
    else lb_u32((unsigned long)v);
}

static void lb_hex32(unsigned long v)
{
    int i;
    static const char hexd[] = "0123456789abcdef";
    lb_str("0x");
    for (i = 7; i >= 0; i--) lb_ch(hexd[(v >> (i * 4)) & 0xF]);
}

static void lb_mode(unsigned int m)
{
    int i;
    lb_ch('0');
    for (i = 2; i >= 0; i--) lb_ch((char)('0' + ((m >> (i * 3)) & 0x7)));
}

/* Six-digit zero-padded sequence number, subtraction only. */
static void lb_seq6(unsigned long v)
{
    int k;
    for (k = 4; k < 10; k++) {
        unsigned long p = dbg_pow10[k];
        unsigned int d = 0;
        while (v >= p) { v -= p; d++; }
        lb_ch((char)('0' + d));
    }
}

static void lb_path(const char *p)
{
    long i = 0;
    if (!p) { lb_str("<null>"); return; }
    lb_ch('"');
    while (p[i] && i < LOG_PATH_CHARS) { lb_ch(p[i]); i++; }
    if (p[i]) lb_str("...");
    lb_ch('"');
}

static void log_open_once(void)
{
    long fd;
    if (g_log_state != 0) return;
    fd = sys_open(LOG_PATH, LOG_OPEN_FLAGS, LOG_MODE);
    if (fd < 0) {
        g_log_state = 2;
        return;
    }
    g_log_fd = (int)fd;
    g_log_state = 1;
}

/* Caller must hold g_log_busy. Writes the built line with a single
 * raw write; any failure marks the log dead. NOTE: never closes
 * g_log_fd here — if the fd number was recycled for one of rmm's own
 * files (fd closed by a path that bypasses our wrapper), closing it
 * would close rmm's live fd. A leaked, never-again-used log fd is
 * harmless. A short write also marks the log dead (a truncated line
 * must never be followed by the next entry on the same line). */
static void log_flush(void)
{
    long w;
    if (g_log_state == 2) return;
    if (g_log_count >= LOG_CAP) {
        g_log_state = 2;
        return;
    }
    if (g_log_state == 0) log_open_once();
    if (g_log_state == 1) {
        w = sys_write(g_log_fd, (long)g_logbuf, g_loglen);
        if (w != g_loglen) {
            g_log_state = 2;
        } else {
            g_log_count++;
        }
    }
}

/* Begin a line: take the guard, reset the buffer, emit the sequence
 * number. Returns 0 if this entry must be dropped. The guard is an
 * ATOMIC test-and-set (ldrex/strex on ARMv7-a, no mutex, no libcall):
 * a plain check-then-set on a volatile int lets two threads in at
 * once and they would interleave into the shared buffer. With the
 * atomic guard, a contending entry is dropped, never corrupted.
 * The guard also serializes log_flush(), so log_open_once() (only
 * ever called from log_flush) cannot race either. */
static int log_start(void)
{
    if (__sync_lock_test_and_set(&g_log_busy, 1)) return 0;
    if (g_log_state == 2) {
        __sync_lock_release(&g_log_busy);
        return 0;
    }
    lb_reset();
    lb_ch('[');
    lb_seq6((unsigned long)g_log_count);
    lb_str("] ");
    return 1;
}

static void log_end(void)
{
    lb_ch('\n');
    log_flush();
    __sync_lock_release(&g_log_busy);
}

static int log_errno_now(void)
{
    int *e = __errno_location();
    return e ? *e : 0;
}

static void log_open_call(const char *fn, const char *path,
                          long flags, long mode, long ret)
{
    if (!log_start()) return;
    lb_str(fn); lb_ch('('); lb_path(path);
    lb_str(", flags="); lb_hex32((unsigned long)flags);
    lb_str(", mode="); lb_mode((unsigned int)mode);
    lb_str(") = "); lb_i32(ret);
    lb_str(" errno="); lb_i32((long)log_errno_now());
    log_end();
}

static void log_openat_call(int dirfd, const char *path,
                            long flags, long mode, long ret)
{
    if (!log_start()) return;
    lb_str("openat("); lb_i32((long)dirfd); lb_str(", "); lb_path(path);
    lb_str(", flags="); lb_hex32((unsigned long)flags);
    lb_str(", mode="); lb_mode((unsigned int)mode);
    lb_str(") = "); lb_i32(ret);
    lb_str(" errno="); lb_i32((long)log_errno_now());
    log_end();
}

static void log_ioctl_call(int fd, unsigned long request, long arg, long ret)
{
    if (!log_start()) return;
    lb_str("ioctl("); lb_i32((long)fd);
    lb_str(", req="); lb_hex32(request);
    lb_str(", arg="); lb_hex32((unsigned long)arg);
    lb_str(") = "); lb_i32(ret);
    lb_str(" errno="); lb_i32((long)log_errno_now());
    log_end();
}

static void log_close_call(int fd, long ret)
{
    if (!log_start()) return;
    lb_str("close("); lb_i32((long)fd);
    lb_str(") = "); lb_i32(ret);
    lb_str(" errno="); lb_i32((long)log_errno_now());
    log_end();
}

/* Marker for the one case where the log must record its own end: the
 * host closed our log fd. Written BEFORE the forward (the fd is still
 * open), then the log is marked dead so the normal post-forward log
 * attempt for this close is dropped. The line is a marker, not a
 * call entry — it does not match the [NNNNNN] fn(...) pattern. */
static void log_marker_logfd_closed(int fd)
{
    if (!log_start()) return;
    lb_str("!! log fd ");
    lb_i32((long)fd);
    lb_str(" closed by host - logging stops here");
    log_end();
}

/* ---------------------------------------------------------------- */
/* Intercepted PLT symbols. Forward via raw syscall FIRST, then do   */
/* the shim's own bookkeeping. Never call the intercepted names      */
/* internally (that would recurse into ourselves).                   */
/* ---------------------------------------------------------------- */

static int do_open_track(const char *path, long r)
{
    if (r >= 0 && path) {
        if (s_eq(path, AO_DEV)) g_ao_fd = (int)r;
        else if (s_eq(path, CPLD_DEV)) g_cpld_fd = (int)r;
    }
    if (r < 0) {
        set_errno(r);
        return -1;
    }
    return (int)r;
}

int open(const char *path, int flags, unsigned int mode)
{
    long r;
    int ret;
    ensure_init();
    r = sys_open(path, (long)flags, (long)mode);
    ret = do_open_track(path, r);
    log_open_call("open", path, (long)flags, (long)mode, (long)ret);
    return ret;
}

int open64(const char *path, int flags, unsigned int mode)
{
    long r;
    int ret;
    ensure_init();
    r = sys_open(path, (long)flags, (long)mode);
    ret = do_open_track(path, r);
    log_open_call("open64", path, (long)flags, (long)mode, (long)ret);
    return ret;
}

int openat(int dirfd, const char *path, int flags, unsigned int mode)
{
    long r;
    int ret;
    ensure_init();
    r = raw_sys(SYS_OPENAT, (long)dirfd, (long)path, (long)flags, (long)mode, 0, 0);
    ret = do_open_track(path, r);
    log_openat_call(dirfd, path, (long)flags, (long)mode, (long)ret);
    return ret;
}

int ioctl(int fd, unsigned long request, void *arg)
{
    long r;
    int ret;
    ensure_init();
    r = sys_ioctl(fd, request, (long)arg);
    if (r >= 0 && fd >= 0 && fd == g_ao_fd) {
        if (request == AO_SETPUBATTR || request == AO_ENABLECHN
#ifdef AO_SHIM_TEST_HOOK
            || request == 0x5401UL /* TCGETS: test hook only, see header */
#endif
            ) {
            maybe_activate();
        }
    }
    if (r < 0) {
        set_errno(r);
        ret = -1;
    } else {
        ret = (int)r;
    }
    log_ioctl_call(fd, request, (long)arg, (long)ret);
    return ret;
}

int close(int fd)
{
    long r;
    int ret;
    ensure_init();
    if (fd >= 0 && fd == g_ao_fd) {
        if (g_amp_on) amp_off();
        g_ao_fd = -1;
    }
    if (fd >= 0 && fd == g_cpld_fd) {
        g_cpld_fd = -1;
        g_cpld_ours = 0;
    }
    if (fd >= 0 && fd == g_log_fd && g_log_state == 1) {
        log_marker_logfd_closed(fd);
        g_log_state = 2;
        g_log_fd = -1;
    }
    r = sys_close(fd);
    if (r < 0) {
        set_errno(r);
        ret = -1;
    } else {
        ret = (int)r;
    }
    log_close_call(fd, (long)ret);
    return ret;
}
