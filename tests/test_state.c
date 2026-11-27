/* Save/load round trip for CPU state and the three memory regions.
 * Synthetic values only, no game data. */

#include "psprecomp/state.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/recomp_rt.h"

#include <stdio.h>
#include <string.h>

#define PATH "./test-state.bin"

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

int main(void) {
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    psp_cpu_reset();

    psp_cpu.r[PSP_REG_V0] = 0x12345678u;
    psp_cpu.r[PSP_REG_SP] = 0x088F0000u;
    psp_cpu.hi = 0xAA55AA55u;
    psp_cpu.lo = 0x5AA55AA5u;
    psp_cpu.pc = 0x08804000u;
    psp_cpu.f[3] = 12.5f;
    psp_cpu.fcr31 = 0x00800000u;
    psp_cpu.v[7] = -2.0f;
    psp_cpu.vfpu_cc = 0x5Au;

    psp_write32(PSP_RAM_BASE + 0x1000, 0xDEADBEEFu);
    psp_write32(PSP_VRAM_BASE + 0x2000, 0x00FF00FFu);
    psp_write8(PSP_SCRATCH_BASE + 0x10, 0x7Fu);

    CHECK(psp_state_save(PATH) == 0, "state saved");

    /* Trample everything the snapshot owns. */
    memset(psp_cpu.r, 0, sizeof psp_cpu.r);
    memset(psp_cpu.f, 0, sizeof psp_cpu.f);
    memset(psp_cpu.v, 0, sizeof psp_cpu.v);
    psp_cpu.hi = psp_cpu.lo = psp_cpu.pc = psp_cpu.fcr31 = psp_cpu.vfpu_cc = 0;
    memset(psp_mem.ram, 0, PSP_RAM_SIZE);
    memset(psp_mem.vram, 0, PSP_VRAM_SIZE);
    memset(psp_mem.scratch, 0, PSP_SCRATCH_SIZE);

    CHECK(psp_state_load(PATH) == 0, "state loaded");
    CHECK(psp_cpu.r[PSP_REG_V0] == 0x12345678u, "v0 restored: 0x%08X", psp_cpu.r[PSP_REG_V0]);
    CHECK(psp_cpu.r[PSP_REG_SP] == 0x088F0000u, "sp restored");
    CHECK(psp_cpu.r[0] == 0, "$zero stays zero after load");
    CHECK(psp_cpu.hi == 0xAA55AA55u && psp_cpu.lo == 0x5AA55AA5u, "HI/LO restored");
    CHECK(psp_cpu.pc == 0x08804000u, "PC restored");
    CHECK(psp_cpu.f[3] == 12.5f, "FPU register restored");
    CHECK(psp_cpu.fcr31 == 0x00800000u, "FPU control restored");
    CHECK(psp_cpu.v[7] == -2.0f, "VFPU register restored");
    CHECK(psp_cpu.vfpu_cc == 0x5Au, "VFPU condition restored");
    CHECK(psp_read32(PSP_RAM_BASE + 0x1000) == 0xDEADBEEFu, "main RAM restored");
    CHECK(psp_read32(PSP_VRAM_BASE + 0x2000) == 0x00FF00FFu, "VRAM restored");
    CHECK(psp_read8(PSP_SCRATCH_BASE + 0x10) == 0x7Fu, "scratchpad restored");
    CHECK(psp_cpu.frame == NULL && psp_cpu.unwind == 0, "host-only fields reset");

    /* A truncated file must be rejected, not half-loaded. */
    FILE *f = fopen(PATH, "wb");
    if (f) { fwrite("bad", 1, 3, f); fclose(f); }
    CHECK(psp_state_load(PATH) != 0, "truncated state rejected");

    remove(PATH);
    psp_mem_free();
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("state checks passed (synthetic values, no game data)\n");
    return 0;
}
