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

static void sink(const int16_t *stereo, uint32_t frames, uint32_t rate, void *user) {
    (void)stereo; (void)user;
    got_calls++;
    got_frames += frames;
    got_rate = rate;
}

static void sas_core(void) {
    psp_cpu.r[PSP_REG_A0] = 0;
    psp_cpu.r[PSP_REG_A1] = OUT;
    psp_cpu.r[PSP_REG_A2] = 0;
    psp_cpu.r[PSP_REG_A3] = 0;
    psp_hle_call(SAS_CORE);
}

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

    psp_mem_free();
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("audio sink checks passed (synthetic grain, no game data)\n");
    return 0;
}
