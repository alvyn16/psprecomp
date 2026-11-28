/* psprecomp — the smaller firmware libraries.
 *
 * Kernel_Library, UtilsForUser, StdioForUser, sceSuspendForUser,
 * LoadExecForUser, ModuleMgrForUser, sceCtrl and sceAudio. Individually small,
 * but collectively they are what a game's C runtime needs before main() gets
 * anywhere -- newlib's reentrancy setup alone wants interrupt masking, a
 * clock, and the standard file descriptors.
 */

#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  define misc_mkdir(p) _mkdir(p)
#else
#  include <stdatomic.h>
#  include <sys/stat.h>
#  define misc_mkdir(p) mkdir(p, 0755)
#endif

/* ---- Kernel_Library ------------------------------------------------------
 * Interrupt masking. The mask belongs to the *thread* that opened the critical
 * section, not to the machine: with several guest threads on one CPU, a global
 * flag would let one thread's suspend silence interrupts inside another
 * thread's unrelated bracket. The scheduler owns that state, because it is
 * also what decides whether an interrupt may be delivered at a park. */

static void hle_CpuSuspendIntr(void) {
    psp_ret((uint32_t)psp_threadman_intr_suspend());   /* the cookie resume expects */
}

static void hle_CpuResumeIntr(void) {
    psp_threadman_intr_resume(psp_arg(0));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- UtilsForUser -------------------------------------------------------- */

/* Cache maintenance. There is no cache to write back -- the recompiled code
 * and the GE share one flat backing store -- so these are genuinely no-ops
 * rather than unimplemented. */
static void hle_CacheOp(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_LibcTime(void) {
    time_t t = time(NULL);
    uint32_t out = psp_arg(0);
    if (out) psp_write32(out, (uint32_t)t);
    psp_ret((uint32_t)t);
}

static void hle_LibcClock(void) {
    /* Microseconds since start. A game that uses this for frame pacing needs
     * it to advance, so it is derived from the host clock rather than being a
     * constant -- a frozen clock makes a game either spin or run at infinite
     * speed, both of which look like a hang. */
    psp_ret((uint32_t)((uint64_t)clock() * 1000000ull / CLOCKS_PER_SEC));
}

static void hle_LibcGettimeofday(void) {
    uint32_t tv = psp_arg(0);
    if (tv) {
        time_t t = time(NULL);
        psp_write32(tv, (uint32_t)t);
        psp_write32(tv + 4, 0);
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* General-purpose I/O pins, wired to the debug board. Nothing is connected.
 * GPO is write-only on hardware; keeping the last value lets a later GetGPO
 * answer consistently, and stops the engine's start-up write from being
 * reported as a missing firmware call. */
static uint32_t g_gpo;
static void hle_GetGPI(void)  { psp_ret(0); }
static void hle_SetGPO(void)  { g_gpo = psp_arg(0) & 0xFFu; psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_GetGPO(void)  { psp_ret(g_gpo); }

/* ---- StdioForUser -------------------------------------------------------- */
/* These return the file descriptors, which sceIoWrite then recognises. */
static void hle_Stdin(void)  { psp_ret(0); }
static void hle_Stdout(void) { psp_ret(1); }
static void hle_Stderr(void) { psp_ret(2); }

/* ---- sceSuspendForUser --------------------------------------------------- */
/* Power management around suspend. Nothing suspends here. */
static void hle_ok(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- LoadExecForUser ----------------------------------------------------- */

static int g_exit_requested;
int psp_exit_requested(void) { return g_exit_requested; }

static void hle_ExitGame(void) {
    /* A game calling this is finished. Recording it rather than terminating
     * the process lets the host report what happened and dump its counters. */
    g_exit_requested = 1;
    fprintf(stderr, "psprecomp: the game called sceKernelExitGame\n");
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RegisterExitCallback(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- ModuleMgrForUser ---------------------------------------------------- */
/* A game-sharing microgame is self-contained and does not load further
 * modules, so these report a plausible identity rather than doing anything.
 * A title that genuinely loads PRXs at run time will need real ones. */
/* These report the id of the one loaded module.
 *
 * A previous note here recorded the opposite -- that returning an id was a
 * "plausible lie" and an error was the truthful answer -- on the strength of a
 * test showing byte-identical output either way. **That test was run while
 * `jal` never assigned `$ra`**, so every non-leaf function in the program was
 * returning through a stale register. A falsification obtained under broken
 * codegen is not a falsification.
 *
 * Re-run after that fix, the two answers differ clearly: returning an id makes
 * the game's own "libc:_getmodreent: no reent structure" diagnostic disappear
 * and drops bad memory accesses from 3 to 0.
 *
 * And an id is the *truthful* answer. The question is "which module owns this
 * address", a self-contained microgame is exactly one module, and the host has
 * loaded it. Reporting 1 states that; reporting UNKNOWN_MODULE denies a module
 * that demonstrably exists. */
#define SCE_KERNEL_ERROR_UNKNOWN_MODULE 0x80020139u
#define PSP_MAIN_MODULE_ID 1u

static void hle_GetModuleId(void)          { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_GetModuleIdByAddress(void) { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_ModuleOk(void)             { psp_ret(SCE_KERNEL_ERROR_OK); }

/* sceKernelStopUnloadSelfModuleWithStatus -- the NID was observed in Dante's
 * Inferno's libstub table, and PPSSPP's table names it; the name is not
 * SHA-1-derivable, so it is registered unnamed rather than under a name this
 * toolkit cannot verify.
 *
 * The firmware does not return from this call: it stops and unloads the calling
 * module, which is the end of the program. Returning 0 is not a harmless stub
 * because the caller is compiled to continue -- _exit's wrapper leaves the exit
 * status in $a0 and falls straight into code that treats $a0 as a pointer, and
 * Dante's Inferno faults at 0x0D there. Record the exit and unwind the calling
 * thread exactly as sceKernelExitThread does. */
static void hle_StopUnloadSelfModuleWithStatus(void) {
    uint32_t status = psp_arg(0);
    g_exit_requested = 1;
    fprintf(stderr,
            "psprecomp: the game called sceKernelStopUnloadSelfModuleWithStatus(%u)\n",
            status);
    psp_cpu.r[PSP_REG_A0] = status;
    psp_hle_call(0xAA73C935u);                 /* sceKernelExitThread */
    psp_ret(status);                           /* only with nothing to unwind to */
}

/* ---- sceCtrl ------------------------------------------------------------- */

static uint32_t g_buttons;
static uint8_t  g_analog_x = 128, g_analog_y = 128;
static uint32_t g_ctrl_frame;
static uint32_t g_latch[4];  /* PSPSDK SceCtrlLatch: make, break, press, release */
static uint32_t g_latch_samples;

/* The window submits samples on a host thread, independently of the guest.
 * Keep sampling and latch consumption atomic, including in inline mode. */
#ifdef _WIN32
static SRWLOCK g_ctrl_lock = SRWLOCK_INIT;
static void ctrl_lock(void) { AcquireSRWLockExclusive(&g_ctrl_lock); }
static void ctrl_unlock(void) { ReleaseSRWLockExclusive(&g_ctrl_lock); }
#else
static atomic_flag g_ctrl_lock = ATOMIC_FLAG_INIT;
static void ctrl_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_ctrl_lock, memory_order_acquire)) {}
}
static void ctrl_unlock(void) {
    atomic_flag_clear_explicit(&g_ctrl_lock, memory_order_release);
}
#endif

static void ctrl_sample(uint32_t buttons) {
    g_latch[0] |= buttons & ~g_buttons;
    g_latch[1] |= g_buttons & ~buttons;
    g_latch[2] |= buttons;
    g_latch[3] |= ~buttons;
    if (g_latch_samples < INT_MAX) g_latch_samples++;
    g_buttons = buttons;
}

void psp_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay) {
    ctrl_lock();
    ctrl_sample(buttons);
    g_analog_x = ax;
    g_analog_y = ay;
    ctrl_unlock();
}

static void hle_CtrlSet(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void ctrl_latch(int consume) {
    uint32_t out = psp_arg(0), data[4], samples;
    if (!out || !psp_mem_ptr(out, 16)) { psp_ret(0x80010016u); return; }
    ctrl_lock();
    if (consume && !g_latch_samples) {
        /* PSPSDK documents an empty read as blocking until the next sample.
         * Use the existing frame wait rather than inventing a polling thread.
         * A host sample arriving during the wait takes precedence. */
        ctrl_unlock();
        psp_threadman_frame_wait();
        ctrl_lock();
        if (!g_latch_samples) ctrl_sample(g_buttons);
    }
    samples = g_latch_samples;
    memcpy(data, g_latch, sizeof data);
    if (consume) {
        memset(g_latch, 0, sizeof g_latch);
        g_latch_samples = 0;
    }
    ctrl_unlock();
    for (unsigned i = 0; i < 4; i++) psp_write32(out + i * 4, data[i]);
    psp_ret(samples);
}
static void hle_ReadLatch(void) { ctrl_latch(1); }
static void hle_PeekLatch(void) { ctrl_latch(0); }

/* SceCtrlData: u32 timestamp, u32 buttons, u8 lx, u8 ly, then padding to 16. */
static void hle_ReadBufferPositive(void) {
    uint32_t buf = psp_arg(0), count = psp_arg(1);
    if (!count) count = 1;
    ctrl_lock();
    uint32_t frame = g_ctrl_frame, buttons = g_buttons;
    uint8_t ax = g_analog_x, ay = g_analog_y;
    g_ctrl_frame += count;
    ctrl_unlock();
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = buf + i * 16;
        psp_write32(at, frame + i);
        psp_write32(at + 4, buttons);
        psp_write8(at + 8, ax);
        psp_write8(at + 9, ay);
        for (int k = 10; k < 16; k++) psp_write8(at + (uint32_t)k, 0);
    }
    psp_ret(count);
}

/* ---- sceAudio ------------------------------------------------------------ */

#define AUDIO_CHANNELS 8

typedef struct { int reserved; uint32_t samples; uint32_t format; uint32_t lvol; uint32_t rvol; } audio_ch;
static audio_ch g_audio[AUDIO_CHANNELS];
static uint64_t g_audio_blocks;
static int g_audio_output_claimed;
static psp_audio_sink_fn g_sink;
static void *g_sink_user;
static psp_audio_channel_sink_fn g_ch_sink;
static void *g_ch_sink_user;

uint64_t psp_audio_blocks(void) { return g_audio_blocks; }
void psp_audio_sink_set(psp_audio_sink_fn fn, void *user) { g_sink = fn; g_sink_user = user; }
void psp_audio_channel_sink_set(psp_audio_channel_sink_fn fn, void *user) {
    g_ch_sink = fn;
    g_ch_sink_user = user;
}
int psp_audio_output_active(void) { return g_audio_output_claimed; }

void psp_audio_submit(int source, const int16_t *stereo, uint32_t frames, uint32_t rate) {
    if (g_ch_sink) g_ch_sink(source, stereo, frames, rate, g_ch_sink_user);
    else if (g_sink) g_sink(stereo, frames, rate, g_sink_user);
}

static void submit_guest_pcm(int source, uint32_t buf, uint32_t samples,
                             uint32_t lvol, uint32_t rvol, int mono) {
    if (!buf || !samples || (!g_ch_sink && !g_sink)) return;
    const uint32_t bytes = samples * (mono ? 2u : 4u);
    const uint8_t *raw = (const uint8_t *)psp_mem_ptr(buf, bytes);
    if (!raw) return;

    int16_t tmp[512 * 2];
    uint32_t done = 0;
    while (done < samples) {
        uint32_t chunk = samples - done;
        if (chunk > 512u) chunk = 512u;
        for (uint32_t i = 0; i < chunk; i++) {
            int32_t l, r;
            if (mono) {
                int16_t s;
                memcpy(&s, raw + (done + i) * 2u, 2);
                l = ((int32_t)s * (int32_t)lvol) >> 15;
                r = ((int32_t)s * (int32_t)rvol) >> 15;
            } else {
                int16_t sl, sr;
                memcpy(&sl, raw + (done + i) * 4u, 2);
                memcpy(&sr, raw + (done + i) * 4u + 2u, 2);
                l = ((int32_t)sl * (int32_t)lvol) >> 15;
                r = ((int32_t)sr * (int32_t)rvol) >> 15;
            }
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            tmp[i * 2]     = (int16_t)l;
            tmp[i * 2 + 1] = (int16_t)r;
        }
        psp_audio_submit(source, tmp, chunk, 44100u);
        done += chunk;
    }
}

static void hle_ChReserve(void) {
    if (getenv("PSPRECOMP_TRACE_SAS"))
        fprintf(stderr, "audio: ChReserve ch %d samples %u format %u\n",
                (int)psp_arg(0), psp_arg(1), psp_arg(2));
    /* (channel, samplecount, format) -- channel -1 means "any". */
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0) {
        for (int i = 0; i < AUDIO_CHANNELS; i++)
            if (!g_audio[i].reserved) { ch = i; break; }
    }
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 1;
    g_audio[ch].samples = psp_arg(1);
    g_audio[ch].format  = psp_arg(2);
    g_audio[ch].lvol    = 0x8000u;
    g_audio[ch].rvol    = 0x8000u;
    g_audio_output_claimed = 1;
    psp_ret((uint32_t)ch);
}

static void hle_ChRelease(void) {
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static uint32_t output_channel_pcm(uint32_t ch, uint32_t lvol, uint32_t rvol, uint32_t buf) {
    g_audio_blocks++;
    if (ch >= AUDIO_CHANNELS) return 0;
    g_audio_output_claimed = 1;
    uint32_t lv = (int32_t)lvol >= 0 ? lvol : g_audio[ch].lvol;
    uint32_t rv = (int32_t)rvol >= 0 ? rvol : g_audio[ch].rvol;
    g_audio[ch].lvol = lv;
    g_audio[ch].rvol = rv;
    submit_guest_pcm((int)ch, buf, g_audio[ch].samples, lv, rv, g_audio[ch].format == 0x10);
    return g_audio[ch].samples;
}

static void hle_OutputPanned(void) {
    psp_ret(output_channel_pcm(psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3)));
}

/* The blocking forms hold the caller while the hardware drains a buffer, and
 * that is what paces a game's audio thread. Returning at once lets a mixer
 * loop spin forever and starve every other guest thread.
 *
 * Under real threads the hardware queue is modelled on the host clock: a
 * buffer is due one play time after the last, and the call only waits while
 * the queue is more than a buffer ahead. Waiting a fixed play time per call
 * did not work there. The scheduler is cooperative, each tick of the wait
 * handed the CPU to the game's main thread for a whole frame, and WTF's audio
 * ran at about 10 buffers a second instead of 172; its title waits for a
 * jingle that, by its own sample count, never finished. A thread that falls
 * behind now catches up without waiting, as it would against a real queue.
 * Inline mode keeps the virtual-time wait. */
#if PSPRECOMP_REAL_THREADS
static uint64_t host_now_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static uint64_t g_audio_due_us[AUDIO_CHANNELS];
#endif

static void wait_channel_pacing(uint32_t ch, uint32_t samples) {
    const uint64_t play_us = (uint64_t)samples * 1000000u / 44100u;
#if PSPRECOMP_REAL_THREADS
    if (ch < AUDIO_CHANNELS) {
        uint64_t now = host_now_us();
        if (g_audio_due_us[ch] < now) g_audio_due_us[ch] = now;   /* underrun */
        g_audio_due_us[ch] += play_us;
        while (host_now_us() + play_us < g_audio_due_us[ch]) psp_threadman_yield();
    }
#else
    (void)ch;
    psp_threadman_delay(play_us);
#endif
}

static void hle_OutputBlocking(void) {
    uint32_t ch = psp_arg(0);
    uint32_t vol = psp_arg(1);
    uint32_t buf = psp_arg(2);
    uint32_t samples = output_channel_pcm(ch, vol, vol, buf);
    static int traced;
    if (getenv("PSPRECOMP_TRACE_SAS") && traced++ < 4)
        fprintf(stderr, "audio: OutputBlocking ch %u samples %u buf 0x%08X\n", ch, samples, buf);
    wait_channel_pacing(ch, samples);
    psp_ret(samples);
}

static void hle_OutputPannedBlocking(void) {
    uint32_t ch = psp_arg(0);
    uint32_t samples = output_channel_pcm(ch, psp_arg(1), psp_arg(2), psp_arg(3));
    static int traced;
    if (getenv("PSPRECOMP_TRACE_SAS") && traced++ < 4)
        fprintf(stderr, "audio: OutputPannedBlocking ch %u samples %u buf 0x%08X\n", ch, samples, psp_arg(3));
    wait_channel_pacing(ch, samples);
    psp_ret(samples);
}

/* sceAudio's second output API has no channel argument: one output is reserved
 * with its sample count and every later Output2 call drains it. Dante's
 * Inferno's FMOD mixer uses this rather than the channel API above, and with
 * the NID unimplemented its thread looped at nearly a million calls a second,
 * starving the game's other threads. The same pacing applies here. */
static uint32_t g_output2_samples;
#if PSPRECOMP_REAL_THREADS
static uint64_t g_output2_due_us;
#endif

static void hle_Output2Reserve(void) {
    g_output2_samples = psp_arg(0);
    g_audio_output_claimed = 1;
    if (getenv("PSPRECOMP_TRACE_SAS"))
        fprintf(stderr, "audio: Output2Reserve samples %u\n", g_output2_samples);
    psp_ret(0);                       /* the single output's id on success */
}

static void hle_Output2OutputBlocking(void) {
    /* (volume, buf) -- the return value is the sample count played. */
    uint32_t vol = psp_arg(0);
    uint32_t buf = psp_arg(1);
    uint32_t samples = g_output2_samples ? g_output2_samples : 256u;
    g_audio_blocks++;
    g_audio_output_claimed = 1;
    static int traced;
    if (getenv("PSPRECOMP_TRACE_SAS") && traced++ < 4)
        fprintf(stderr, "audio: Output2OutputBlocking vol 0x%X samples %u buf 0x%08X\n",
                vol, samples, buf);
    uint32_t v = (int32_t)vol >= 0 ? vol : 0x8000u;
    submit_guest_pcm(8, buf, samples, v, v, 0);
    const uint64_t play_us = (uint64_t)samples * 1000000u / 44100u;
#if PSPRECOMP_REAL_THREADS
    uint64_t now = host_now_us();
    if (g_output2_due_us < now) g_output2_due_us = now;
    g_output2_due_us += play_us;
    while (host_now_us() + play_us < g_output2_due_us) psp_threadman_yield();
#else
    psp_threadman_delay(play_us);
#endif
    psp_ret(samples);
}

/* The sample count a channel outputs per call. It is set at reserve time and
 * changed here; ignoring the change left WTF's channel at its reserved length,
 * so each blocking output waited for 4,096 samples while the game mixed 256,
 * and its audio ran 16 times slow. */
static void hle_SetChannelDataLen(void) {
    uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(0x80260003); return; }   /* invalid channel */
    g_audio[ch].samples = psp_arg(1);
    if (getenv("PSPRECOMP_TRACE_SAS"))
        fprintf(stderr, "audio: SetChannelDataLen ch %u samples %u\n", ch, psp_arg(1));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Zero remaining means "ready for more", so a game's audio loop keeps going. */
static void hle_GetChannelRestLength(void) { psp_ret(0); }

static int      g_vmem_locked;
static uint32_t g_umd_cbid;
static uint64_t g_rtc_start_ns;

static uint64_t rtc_now_ns(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void hle_IsCpuIntrEnable(void) {
    psp_ret(psp_threadman_intr_disabled() ? 0u : 1u);
}

static void hle_VolatileMemLock(int trylock) {
    uint32_t type = psp_arg(0);
    uint32_t addr_ptr = psp_arg(1);
    uint32_t size_ptr = psp_arg(2);
    if (type != 0) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
    if (trylock && g_vmem_locked) { psp_ret(0x802B0200u); return; }
    g_vmem_locked = 1;
    if (addr_ptr) psp_write32(addr_ptr, 0x08400000u);
    if (size_ptr) psp_write32(size_ptr, 0x00400000u);
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_VolatileMemTryLock(void) { hle_VolatileMemLock(1); }
static void hle_VolatileMemLockBlocking(void) { hle_VolatileMemLock(0); }
static void hle_VolatileMemUnlock(void) { g_vmem_locked = 0; psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_DmacMemcpy(void) {
    uint32_t dst = psp_arg(0), src = psp_arg(1), size = psp_arg(2);
    void *d = psp_mem_ptr(dst, size);
    const void *s = psp_mem_ptr(src, size);
    if (d && s) {
        memmove(d, s, size);
    } else {
        for (uint32_t i = 0; i < size; i++) psp_write8(dst + i, psp_read8(src + i));
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

#define PSP_UMD_PRESENT  0x02u
#define PSP_UMD_READY    0x20u
#define PSP_UMD_READABLE 0x10u

static void hle_UmdCheckMedium(void) { psp_ret(1u); }
static void hle_UmdActivate(void) {
    if (g_umd_cbid) {
        uint32_t saved_a0 = psp_cpu.r[PSP_REG_A0], saved_a1 = psp_cpu.r[PSP_REG_A1];
        psp_cpu.r[PSP_REG_A0] = g_umd_cbid;
        psp_cpu.r[PSP_REG_A1] = PSP_UMD_PRESENT | PSP_UMD_READY | PSP_UMD_READABLE;
        psp_hle_call(0xC11BA8C4u); /* sceKernelNotifyCallback */
        psp_cpu.r[PSP_REG_A0] = saved_a0;
        psp_cpu.r[PSP_REG_A1] = saved_a1;
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_UmdGetDriveStat(void) {
    psp_ret(PSP_UMD_PRESENT | PSP_UMD_READY | PSP_UMD_READABLE);
}
static void hle_UmdRegisterCallback(void) {
    uint32_t cbid = psp_arg(1);
    if (!cbid) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    g_umd_cbid = cbid;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RtcGetCurrentTick(void) {
    uint32_t tick_ptr = psp_arg(0);
    if (!tick_ptr) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
    uint64_t now = rtc_now_ns();
    static uint64_t s_last_ns = 0;
    static uint32_t s_spin_hits = 0;
    if (now >= s_last_ns && (now - s_last_ns) < 4000ull) {
        if (++s_spin_hits >= 64u) {
            s_spin_hits = 0;
            psp_threadman_delay(200);
            now = rtc_now_ns();
        }
    } else {
        s_spin_hits = 0;
    }
    s_last_ns = now;
    if (!g_rtc_start_ns) g_rtc_start_ns = now;
    uint64_t us = (now - g_rtc_start_ns) / 1000ull;
    psp_write32(tick_ptr, (uint32_t)us);
    psp_write32(tick_ptr + 4, (uint32_t)(us >> 32));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RtcGetCurrentClockLocalTime(void) {
    uint32_t p = psp_arg(0);
    if (!p) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    time_t sec = ts.tv_sec;
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &sec);
#else
    localtime_r(&sec, &tm_buf);
#endif
    psp_write16(p + 0,  (uint16_t)(tm_buf.tm_year + 1900));
    psp_write16(p + 2,  (uint16_t)(tm_buf.tm_mon + 1));
    psp_write16(p + 4,  (uint16_t)tm_buf.tm_mday);
    psp_write16(p + 6,  (uint16_t)tm_buf.tm_hour);
    psp_write16(p + 8,  (uint16_t)tm_buf.tm_min);
    psp_write16(p + 10, (uint16_t)tm_buf.tm_sec);
    psp_write32(p + 12, (uint32_t)(ts.tv_nsec / 1000l));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_UtilityGetSystemParamInt(void) {
    uint32_t id = psp_arg(0), out = psp_arg(1);
    uint32_t val = 0;
    switch (id) {
    case 8:  val = 1; break;  /* PSP_SYSTEMPARAM_ID_INT_LANGUAGE: English */
    case 9:  val = 1; break;  /* PSP_SYSTEMPARAM_ID_INT_BUTTON_PREFERENCE: X confirm */
    default: break;
    }
    if (out) psp_write32(out, val);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_UtilityGetSystemParamString(void) {
    uint32_t out = psp_arg(1);
    uint32_t len = psp_arg(2);
    static const char k_name[] = "PSP";
    if (out && len) {
        uint32_t n = len < sizeof(k_name) ? len - 1u : (uint32_t)(sizeof(k_name) - 1u);
        for (uint32_t i = 0; i < n; i++) psp_write8(out + i, (uint8_t)k_name[i]);
        psp_write8(out + n, 0);
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- sceUtility dialog helpers (Savedata + MsgDialog) -------------------- */

#define PSP_UTILITY_DIALOG_NONE     0
#define PSP_UTILITY_DIALOG_INIT     1
#define PSP_UTILITY_DIALOG_VISIBLE  2
#define PSP_UTILITY_DIALOG_QUIT     3
#define PSP_UTILITY_DIALOG_FINISHED 4

#define SCE_UTILITY_ERROR_BUSY             0x80110007u
#define SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA 0x80110307u

static int g_savedata_status;
static int g_msgdialog_status;

static void read_guest_cstr(uint32_t addr, char *out, size_t cap) {
    if (!cap) return;
    if (!addr) { out[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < cap; i++) {
        if (!psp_mem_ptr(addr + (uint32_t)i, 1)) break;
        char c = (char)psp_read8(addr + (uint32_t)i);
        out[i] = c;
        if (!c) return;
    }
    out[i] = '\0';
}

static void sanitize_component(char *s) {
    for (char *p = s; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') *p = '_';
    }
    if (!strcmp(s, ".") || !strcmp(s, "..")) s[0] = '_';
}

static void ensure_dir_tree(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            misc_mkdir(tmp);
            *p = saved;
        }
    }
    misc_mkdir(tmp);
}

static int savedata_path(const char *game, const char *save, const char *file,
                         char *dir_out, size_t dir_cap,
                         char *file_out, size_t file_cap) {
    char g[32], s[32], f[32];
    snprintf(g, sizeof g, "%s", game[0] ? game : "SAVE");
    snprintf(s, sizeof s, "%s", (save[0] && strcmp(save, "<>") != 0) ? save : "0000");
    snprintf(f, sizeof f, "%s", file[0] ? file : "DATA.BIN");
    sanitize_component(g);
    sanitize_component(s);
    sanitize_component(f);
    int n = snprintf(dir_out, dir_cap, "%s/ms/PSP/SAVEDATA/%s%s",
                     psp_io_get_root(), g, s);
    if (n < 0 || (size_t)n >= dir_cap) return -1;
    if (file_out && file_cap) {
        int m = snprintf(file_out, file_cap, "%s/%s", dir_out, f);
        if (m < 0 || (size_t)m >= file_cap) return -1;
    }
    return 0;
}

static int file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

static uint32_t savedata_load_slot(uint32_t param, const char *game, const char *save,
                                   const char *file) {
    char dir[1024], path[1088];
    if (savedata_path(game, save, file, dir, sizeof dir, path, sizeof path) != 0)
        return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
    FILE *fp = fopen(path, "rb");
    if (!fp) return SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA;

    uint32_t buf = psp_read32(param + 0x74u);
    uint32_t buf_sz = psp_read32(param + 0x78u);
    size_t got = 0;
    if (buf && buf_sz) {
        uint8_t *tmp = (uint8_t *)malloc(buf_sz);
        if (tmp) {
            got = fread(tmp, 1, buf_sz, fp);
            if (got && psp_mem_write_block(buf, tmp, (uint32_t)got) != 0) {
                for (size_t i = 0; i < got; i++) psp_write8(buf + (uint32_t)i, tmp[i]);
            }
            free(tmp);
        }
    } else {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        if (sz > 0) got = (size_t)sz;
    }
    fclose(fp);
    psp_write32(param + 0x7Cu, (uint32_t)got);
    return SCE_KERNEL_ERROR_OK;
}

static uint32_t savedata_save_slot(uint32_t param, const char *game, const char *save,
                                   const char *file) {
    char dir[1024], path[1088];
    if (savedata_path(game, save, file, dir, sizeof dir, path, sizeof path) != 0)
        return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
    ensure_dir_tree(dir);

    uint32_t buf = psp_read32(param + 0x74u);
    uint32_t sz  = psp_read32(param + 0x7Cu);
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0x80110381u;
    if (buf && sz) {
        uint8_t *tmp = (uint8_t *)malloc(sz);
        if (tmp) {
            for (uint32_t i = 0; i < sz; i++) tmp[i] = psp_read8(buf + i);
            fwrite(tmp, 1, sz, fp);
            free(tmp);
        }
    }
    fclose(fp);
    return SCE_KERNEL_ERROR_OK;
}

static uint32_t savedata_pick_list_slot(uint32_t list_ptr, const char *game,
                                        const char *file, int want_existing,
                                        char *chosen, size_t chosen_cap) {
    if (!list_ptr || !psp_mem_ptr(list_ptr, 20)) return 0;
    for (int i = 0; i < 256; i++) {
        char slot[20];
        read_guest_cstr(list_ptr + (uint32_t)(i * 20), slot, sizeof slot);
        if (!slot[0]) break;
        char dir[1024], path[1088];
        if (savedata_path(game, slot, file, dir, sizeof dir, path, sizeof path) == 0) {
            if (file_exists(path) == want_existing) {
                snprintf(chosen, chosen_cap, "%s", slot);
                return 1;
            }
        }
    }
    return 0;
}

static void savedata_write_name(uint32_t param, const char *save) {
    for (size_t i = 0; i < 20; i++) {
        uint8_t c = (i < strlen(save)) ? (uint8_t)save[i] : 0;
        psp_write8(param + 0x4Cu + (uint32_t)i, c);
    }
}

static void hle_UtilitySavedataInitStart(void) {
    uint32_t param = psp_arg(0);
    if (!param || !psp_mem_ptr(param, 0x80)) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
        return;
    }
    if (g_savedata_status != PSP_UTILITY_DIALOG_NONE &&
        g_savedata_status != PSP_UTILITY_DIALOG_FINISHED) {
        psp_ret(SCE_UTILITY_ERROR_BUSY);
        return;
    }

    uint32_t mode = psp_read32(param + 0x30u);
    char game[16], save[24], file[16];
    read_guest_cstr(param + 0x3Cu, game, sizeof game);
    read_guest_cstr(param + 0x4Cu, save, sizeof save);
    read_guest_cstr(param + 0x64u, file, sizeof file);
    uint32_t list_ptr = psp_read32(param + 0x60u);

    uint32_t res = SCE_KERNEL_ERROR_OK;
    switch (mode) {
    case 0: /* AUTOLOAD */
    case 2: /* LOAD */
    case 15: /* READDATASECURE */
    case 16: /* READDATA */
        if ((!save[0] || strcmp(save, "<>") == 0) &&
            savedata_pick_list_slot(list_ptr, game, file, 1, save, sizeof save)) {
            savedata_write_name(param, save);
        }
        res = savedata_load_slot(param, game, save, file);
        break;
    case 4: /* LISTLOAD */
        if (savedata_pick_list_slot(list_ptr, game, file, 1, save, sizeof save)) {
            savedata_write_name(param, save);
            res = savedata_load_slot(param, game, save, file);
        } else {
            res = SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA;
        }
        break;
    case 1: /* AUTOSAVE */
    case 3: /* SAVE */
    case 13: /* MAKEDATASECURE */
    case 14: /* MAKEDATA */
    case 17: /* WRITEDATASECURE */
    case 18: /* WRITEDATA */
        if (!save[0] || strcmp(save, "<>") == 0) {
            if (!savedata_pick_list_slot(list_ptr, game, file, 0, save, sizeof save))
                snprintf(save, sizeof save, "0000");
            savedata_write_name(param, save);
        }
        res = savedata_save_slot(param, game, save, file);
        break;
    case 5: /* LISTSAVE */
        if (!savedata_pick_list_slot(list_ptr, game, file, 0, save, sizeof save)) {
            if (!savedata_pick_list_slot(list_ptr, game, file, 1, save, sizeof save))
                snprintf(save, sizeof save, "0000");
        }
        savedata_write_name(param, save);
        res = savedata_save_slot(param, game, save, file);
        break;
    case 6: /* LISTDELETE */
    case 7: /* DELETE */
    case 9: /* AUTODELETE */
    case 10: /* SINGLEDELETE */
    case 19: /* ERASESECURE */
    case 20: /* ERASE */
    case 21: /* DELETEDATA */ {
        if (mode == 6)
            savedata_pick_list_slot(list_ptr, game, file, 1, save, sizeof save);
        char dir[1024], path[1088];
        if (savedata_path(game, save, file, dir, sizeof dir, path, sizeof path) == 0 &&
            remove(path) == 0) {
            res = SCE_KERNEL_ERROR_OK;
        } else {
            res = SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA;
        }
        break;
    }
    case 8: /* SIZES */
    case 22: /* GETSIZE */ {
        uint32_t ms_free = psp_read32(param + 0x5D0u);
        if (ms_free && psp_mem_ptr(ms_free, 16)) {
            psp_write32(ms_free + 0, 32768u);
            psp_write32(ms_free + 4, 4096u);
            psp_write32(ms_free + 8, 131072u);
        }
        uint32_t size_info = psp_read32(param + 0x5FCu);
        if (size_info && psp_mem_ptr(size_info, 64)) {
            psp_write32(size_info + 36, 4096u);
            psp_write32(size_info + 40, 131072u);
        }
        res = SCE_KERNEL_ERROR_OK;
        break;
    }
    default:
        res = SCE_KERNEL_ERROR_OK;
        break;
    }

    psp_write32(param + 0x1Cu, res);
    g_savedata_status = PSP_UTILITY_DIALOG_INIT;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static int utility_poll_status(int *status) {
    int cur = *status;
    if (cur == PSP_UTILITY_DIALOG_INIT) *status = PSP_UTILITY_DIALOG_VISIBLE;
    else if (cur == PSP_UTILITY_DIALOG_FINISHED) *status = PSP_UTILITY_DIALOG_NONE;
    return cur;
}

static void hle_UtilitySavedataGetStatus(void) {
    psp_ret((uint32_t)utility_poll_status(&g_savedata_status));
}

static void hle_UtilitySavedataUpdate(void) {
    if (g_savedata_status == PSP_UTILITY_DIALOG_VISIBLE)
        g_savedata_status = PSP_UTILITY_DIALOG_QUIT;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_UtilitySavedataShutdownStart(void) {
    if (g_savedata_status != PSP_UTILITY_DIALOG_NONE)
        g_savedata_status = PSP_UTILITY_DIALOG_FINISHED;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_UtilityMsgDialogInitStart(void) {
    uint32_t param = psp_arg(0);
    if (!param || !psp_mem_ptr(param, 48)) {
        psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
        return;
    }
    if (g_msgdialog_status != PSP_UTILITY_DIALOG_NONE &&
        g_msgdialog_status != PSP_UTILITY_DIALOG_FINISHED) {
        psp_ret(SCE_UTILITY_ERROR_BUSY);
        return;
    }
    uint32_t sz = psp_read32(param);
    psp_write32(param + 0x1Cu, SCE_KERNEL_ERROR_OK);
    if (sz >= 0x244u && psp_mem_ptr(param, 0x244u)) {
        uint32_t opts = psp_read32(param + 0x23Cu);
        psp_write32(param + 0x240u, (opts & 0x10u) ? 1u : 3u); /* YES or BACK */
    }
    g_msgdialog_status = PSP_UTILITY_DIALOG_INIT;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_UtilityMsgDialogGetStatus(void) {
    psp_ret((uint32_t)utility_poll_status(&g_msgdialog_status));
}

static void hle_UtilityMsgDialogUpdate(void) {
    if (g_msgdialog_status == PSP_UTILITY_DIALOG_VISIBLE)
        g_msgdialog_status = PSP_UTILITY_DIALOG_QUIT;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_UtilityMsgDialogShutdownStart(void) {
    if (g_msgdialog_status != PSP_UTILITY_DIALOG_NONE)
        g_msgdialog_status = PSP_UTILITY_DIALOG_FINISHED;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_UtilityMsgDialogAbort(void) {
    if (g_msgdialog_status != PSP_UTILITY_DIALOG_NONE)
        g_msgdialog_status = PSP_UTILITY_DIALOG_FINISHED;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_misc_reset(void) {
    g_vmem_locked = 0;
    g_umd_cbid = 0;
    g_rtc_start_ns = 0;
    g_savedata_status = PSP_UTILITY_DIALOG_NONE;
    g_msgdialog_status = PSP_UTILITY_DIALOG_NONE;
    /* The interrupt mask is per thread and lives in the scheduler; a reset
     * here cannot reach another thread's bracket, and should not. */
    g_exit_requested = 0;
    ctrl_lock();
    g_buttons = 0;
    g_analog_x = g_analog_y = 128;
    g_ctrl_frame = 0;
    memset(g_latch, 0, sizeof g_latch);
    g_latch_samples = 0;
    ctrl_unlock();
    memset(g_audio, 0, sizeof g_audio);
#if PSPRECOMP_REAL_THREADS
    memset(g_audio_due_us, 0, sizeof g_audio_due_us);
    g_output2_due_us = 0;
#endif
    g_output2_samples = 0;
    g_audio_output_claimed = 0;
    g_audio_blocks = 0;
}

void psp_misc_init(void) { psp_misc_reset(); }

void psp_misc_register(void) {
    psp_hle_register(0x092968F4, "Kernel_Library", "sceKernelCpuSuspendIntr",  hle_CpuSuspendIntr);
    psp_hle_register(0x5F10D406, "Kernel_Library", "sceKernelCpuResumeIntr",   hle_CpuResumeIntr);
    psp_hle_register(0xB55249D2, "Kernel_Library", "sceKernelIsCpuIntrEnable", hle_IsCpuIntrEnable);

    psp_hle_register(0x79D1C3FA, "UtilsForUser", "sceKernelDcacheWritebackAll",              hle_CacheOp);
    psp_hle_register(0xB435DEC5, "UtilsForUser", "sceKernelDcacheWritebackInvalidateAll",    hle_CacheOp);
    psp_hle_register(0x3EE30821, "UtilsForUser", "sceKernelDcacheWritebackRange",            hle_CacheOp);
    psp_hle_register(0x34B9FA9E, "UtilsForUser", "sceKernelDcacheWritebackInvalidateRange",  hle_CacheOp);
    psp_hle_register(0x27CC57F0, "UtilsForUser", "sceKernelLibcTime",         hle_LibcTime);
    psp_hle_register(0x91E4F6A7, "UtilsForUser", "sceKernelLibcClock",        hle_LibcClock);
    psp_hle_register(0x71EC4271, "UtilsForUser", "sceKernelLibcGettimeofday", hle_LibcGettimeofday);
    psp_hle_register(0x37FB5C42, "UtilsForUser", "sceKernelGetGPI",           hle_GetGPI);
    psp_hle_register(0x6AD345D7, "UtilsForUser", "sceKernelSetGPO",           hle_SetGPO);
    psp_hle_register(0xAF3616C0, "UtilsForUser", "sceKernelGetGPO",           hle_GetGPO);

    psp_hle_register(0x172D316E, "StdioForUser", "sceKernelStdin",  hle_Stdin);
    psp_hle_register(0xA6BAB2E9, "StdioForUser", "sceKernelStdout", hle_Stdout);
    psp_hle_register(0xF78BA90A, "StdioForUser", "sceKernelStderr", hle_Stderr);

    psp_hle_register(0xEADB1BD7, "sceSuspendForUser", "sceKernelPowerLock",          hle_ok);
    psp_hle_register(0x3AEE7261, "sceSuspendForUser", "sceKernelPowerUnlock",        hle_ok);
    psp_hle_register(0x090CCB3F, "sceSuspendForUser", "sceKernelPowerTick",          hle_ok);
    psp_hle_register(0xA14F40B2, "sceSuspendForUser", "sceKernelVolatileMemTryLock", hle_VolatileMemTryLock);
    psp_hle_register(0x3E0271D3, "sceSuspendForUser", "sceKernelVolatileMemLock",    hle_VolatileMemLockBlocking);
    psp_hle_register(0xA569E425, "sceSuspendForUser", "sceKernelVolatileMemUnlock",  hle_VolatileMemUnlock);

    psp_hle_register(0x617F3FE6, "sceDmac", "sceDmacMemcpy", hle_DmacMemcpy);
    psp_hle_register(0x36AA6E91, "sceImpose", "sceImposeSetLanguageMode", hle_ok);

    psp_hle_register(0x46EBB729, "sceUmdUser", "sceUmdCheckMedium",         hle_UmdCheckMedium);
    psp_hle_register(0xC6183D47, "sceUmdUser", "sceUmdActivate",            hle_UmdActivate);
    psp_hle_register(0x6B4A146C, "sceUmdUser", "sceUmdGetDriveStat",        hle_UmdGetDriveStat);
    psp_hle_register(0x8EF08FCE, "sceUmdUser", "sceUmdWaitDriveStat",       hle_ok);
    psp_hle_register(0x4A9E5E29, "sceUmdUser", "sceUmdWaitDriveStatCB",     hle_ok);
    psp_hle_register(0xAEE7404D, "sceUmdUser", "sceUmdRegisterUMDCallBack", hle_UmdRegisterCallback);

    psp_hle_register(0x3F7AD767, "sceRtc", "sceRtcGetCurrentTick",           hle_RtcGetCurrentTick);
    psp_hle_register(0xE7C27D1B, "sceRtc", "sceRtcGetCurrentClockLocalTime", hle_RtcGetCurrentClockLocalTime);

    psp_hle_register(0xA5DA2406, "sceUtility", "sceUtilityGetSystemParamInt",    hle_UtilityGetSystemParamInt);
    psp_hle_register(0x34B78343, "sceUtility", "sceUtilityGetSystemParamString", hle_UtilityGetSystemParamString);
    psp_hle_register(0x2A2B3DE0, "sceUtility", "sceUtilityLoadModule",           hle_ok);
    psp_hle_register(0xE49BFE92, "sceUtility", "sceUtilityUnloadModule",         hle_ok);
    psp_hle_register(0x50C4CD57, "sceUtility", "sceUtilitySavedataInitStart",    hle_UtilitySavedataInitStart);
    psp_hle_register(0x8874DBE0, "sceUtility", "sceUtilitySavedataGetStatus",    hle_UtilitySavedataGetStatus);
    psp_hle_register(0xD4B95FFB, "sceUtility", "sceUtilitySavedataUpdate",       hle_UtilitySavedataUpdate);
    psp_hle_register(0x9790B33C, "sceUtility", "sceUtilitySavedataShutdownStart", hle_UtilitySavedataShutdownStart);
    psp_hle_register(0x2AD8E239, "sceUtility", "sceUtilityMsgDialogInitStart",   hle_UtilityMsgDialogInitStart);
    psp_hle_register(0x9A1C91D7, "sceUtility", "sceUtilityMsgDialogGetStatus",   hle_UtilityMsgDialogGetStatus);
    psp_hle_register(0x95FC253B, "sceUtility", "sceUtilityMsgDialogUpdate",      hle_UtilityMsgDialogUpdate);
    psp_hle_register(0x67AF3428, "sceUtility", "sceUtilityMsgDialogShutdownStart", hle_UtilityMsgDialogShutdownStart);
    psp_hle_register(0x4928BD96, "sceUtility", "sceUtilityMsgDialogAbort",       hle_UtilityMsgDialogAbort);

    psp_hle_register(0x05572A5F, "LoadExecForUser", "sceKernelExitGame",             hle_ExitGame);
    psp_hle_register(0x4AC57943, "LoadExecForUser", "sceKernelRegisterExitCallback", hle_RegisterExitCallback);

    /* Observed but unidentified. The game calls this three times on its
     * heap-setup path, and an unimplemented call returns 0 -- which for a
     * module query means "no module" and makes heap establishment fail.
     * Eighteen plausible ModuleMgr names were tried against SHA-1 with no
     * match, so the name is genuinely unknown and is not invented here. */
    psp_hle_register_unnamed(0xF9275D98, "ModuleMgrForUser", hle_GetModuleId);
    psp_hle_register(0xF0A26395, "ModuleMgrForUser", "sceKernelGetModuleId",          hle_GetModuleId);
    psp_hle_register(0xD8B73127, "ModuleMgrForUser", "sceKernelGetModuleIdByAddress", hle_GetModuleIdByAddress);
    psp_hle_register(0x50F0C1EC, "ModuleMgrForUser", "sceKernelStartModule",          hle_ModuleOk);
    psp_hle_register(0xD1FF982A, "ModuleMgrForUser", "sceKernelStopModule",           hle_ModuleOk);
    psp_hle_register(0x2E0911AA, "ModuleMgrForUser", "sceKernelUnloadModule",         hle_ModuleOk);
    psp_hle_register_unnamed(0x8F2DF740, "ModuleMgrForUser", hle_StopUnloadSelfModuleWithStatus);

    psp_hle_register(0x1F4011E6, "sceCtrl", "sceCtrlSetSamplingMode",     hle_CtrlSet);
    psp_hle_register(0x6A2774F3, "sceCtrl", "sceCtrlSetSamplingCycle",    hle_CtrlSet);
    psp_hle_register(0x1F803938, "sceCtrl", "sceCtrlReadBufferPositive",  hle_ReadBufferPositive);
    psp_hle_register(0x0B588501, "sceCtrl", "sceCtrlReadLatch",           hle_ReadLatch);
    psp_hle_register(0xB1D0E5CD, "sceCtrl", "sceCtrlPeekLatch",           hle_PeekLatch);

    psp_hle_register(0x5EC81C55, "sceAudio", "sceAudioChReserve",            hle_ChReserve);
    psp_hle_register(0x6FC46853, "sceAudio", "sceAudioChRelease",            hle_ChRelease);
    psp_hle_register(0x136CAF51, "sceAudio", "sceAudioOutputBlocking",       hle_OutputBlocking);
    psp_hle_register(0x13F592BC, "sceAudio", "sceAudioOutputPannedBlocking", hle_OutputPannedBlocking);
    psp_hle_register(0xE2D56B2D, "sceAudio", "sceAudioOutputPanned",         hle_OutputPanned);
    psp_hle_register(0xB011922F, "sceAudio", "sceAudioGetChannelRestLength", hle_GetChannelRestLength);
    psp_hle_register(0xCB2E439E, "sceAudio", "sceAudioSetChannelDataLen",    hle_SetChannelDataLen);
    psp_hle_register(0x95FD0C2D, "sceAudio", "sceAudioChangeChannelConfig",  hle_ok);
    psp_hle_register(0xB7E1D8E7, "sceAudio", "sceAudioChangeChannelVolume",  hle_ok);
    psp_hle_register(0x01562BA3, "sceAudio", "sceAudioOutput2Reserve",        hle_Output2Reserve);
    psp_hle_register(0x2D53F36E, "sceAudio", "sceAudioOutput2OutputBlocking", hle_Output2OutputBlocking);
}
