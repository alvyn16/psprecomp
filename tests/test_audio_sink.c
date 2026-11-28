/* The host audio sink receives the same grain the SAS mixer writes to guest
 * memory. Synthetic HLE call, no game data. */

#include "psprecomp/hle.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"

#include <stdio.h>

#define SAS_CORE 0xA3589D81u   /* __sceSasCore */
#define OUT      0x08810000u

static int failures;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
            failures++;                                         \
        }                                                       \
    } while (0)

static uint32_t got_frames, got_rate, got_calls;
static int16_t got_first_l, got_first_r;
static int got_src;

static void sink(const int16_t *stereo, uint32_t frames, uint32_t rate, void *user) {
    (void)user;
    got_calls++;
    got_frames += frames;
    got_rate = rate;
    if (frames > 0) { got_first_l = stereo[0]; got_first_r = stereo[1]; }
}

static void ch_sink(int source, const int16_t *stereo, uint32_t frames, uint32_t rate, void *user) {
    got_src = source;
    sink(stereo, frames, rate, user);
}

static uint32_t call_nid(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    psp_cpu.r[PSP_REG_A0] = a0;
    psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_A2] = a2;
    psp_cpu.r[PSP_REG_A3] = a3;
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}

static void sas_core(void) { call_nid(SAS_CORE, 0, OUT, 0, 0); }

int main(void) {
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    psp_cpu_reset();
    psp_hle_init();
    psp_sas_init();

    psp_audio_sink_set(sink, NULL);
    sas_core();
    CHECK(got_calls == 1, "sink invoked once per grain, got %u", got_calls);
    CHECK(got_frames == 256, "default grain is 256 frames, got %u", got_frames);
    CHECK(got_rate == 44100, "default rate is 44100, got %u", got_rate);
    CHECK(psp_sas_frames() == 1, "guest mix still happened");

    psp_audio_sink_set(NULL, NULL);
    sas_core();
    CHECK(got_calls == 1, "detached sink is not called again, got %u", got_calls);

    /* sceAudioOutputPannedBlocking + sceAudioOutput2OutputBlocking route guest PCM
     * and suppress duplicate SAS submission once hardware output is reserved. */
    psp_audio_channel_sink_set(ch_sink, NULL);
    uint32_t ch = call_nid(0x5EC81C55u /* sceAudioChReserve */, 0, 64, 0, 0);
    CHECK(ch == 0, "reserved channel 0, got %u", ch);
    sas_core();
    CHECK(got_calls == 1, "SAS suppressed after output channel reserved, got %u", got_calls);

    psp_write16(OUT + 0, (uint16_t)(int16_t)2000);
    psp_write16(OUT + 2, (uint16_t)(int16_t)-4000);
    call_nid(0x13F592BCu /* sceAudioOutputPannedBlocking */, 0, 0x8000u, 0x4000u, OUT);
    CHECK(got_calls == 2 && got_src == 0 && got_frames == 256 + 64,
          "panned channel emitted 64 frames on src 0");
    CHECK(got_first_l == 2000 && got_first_r == -2000,
          "panned volume scaled L/R correctly (%d, %d)", got_first_l, got_first_r);

    call_nid(0x01562BA3u /* sceAudioOutput2Reserve */, 128, 0, 0, 0);
    psp_write16(OUT + 0, (uint16_t)(int16_t)12000);
    psp_write16(OUT + 2, (uint16_t)(int16_t)-6000);
    call_nid(0x2D53F36Eu /* sceAudioOutput2OutputBlocking */, 0x4000u, OUT, 0, 0);
    CHECK(got_calls == 3 && got_src == 8 && got_frames == 256 + 64 + 128,
          "output2 emitted 128 frames on src 8");
    CHECK(got_first_l == 6000 && got_first_r == -3000,
          "output2 volume scaled L/R correctly (%d, %d)", got_first_l, got_first_r);

    psp_audio_channel_sink_set(NULL, NULL);

    psp_mem_free();
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("audio sink checks passed (synthetic grain, no game data)\n");
    return 0;
}
