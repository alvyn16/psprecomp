/* psprecomp — save states. See include/psprecomp/state.h. */

#include "psprecomp/state.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t magic, version;
    uint32_t r[PSP_NUM_GPR];
    uint32_t hi, lo, pc;
    float    f[32];
    uint32_t fcr31;
    float    v[128];
    uint32_t vfpu_cc;
    uint32_t ram_size, vram_size, scratch_size;
} state_head;

static int write_all(FILE *f, const void *p, size_t n) {
    return fwrite(p, 1, n, f) == n;
}

static int read_all(FILE *f, void *p, size_t n) {
    return fread(p, 1, n, f) == n;
}

int psp_state_save(const char *path) {
    if (!path || !psp_mem.ram || !psp_mem.vram || !psp_mem.scratch) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    state_head h;
    memset(&h, 0, sizeof h);
    h.magic = PSP_STATE_MAGIC;
    h.version = PSP_STATE_VERSION;
    memcpy(h.r, psp_cpu.r, sizeof h.r);
    h.hi = psp_cpu.hi;
    h.lo = psp_cpu.lo;
    h.pc = psp_cpu.pc;
    memcpy(h.f, psp_cpu.f, sizeof h.f);
    h.fcr31 = psp_cpu.fcr31;
    memcpy(h.v, psp_cpu.v, sizeof h.v);
    h.vfpu_cc = psp_cpu.vfpu_cc;
    h.ram_size = PSP_RAM_SIZE;
    h.vram_size = PSP_VRAM_SIZE;
    h.scratch_size = PSP_SCRATCH_SIZE;

    int ok = write_all(f, &h, sizeof h) &&
             write_all(f, psp_mem.ram, h.ram_size) &&
             write_all(f, psp_mem.vram, h.vram_size) &&
             write_all(f, psp_mem.scratch, h.scratch_size);
    return fclose(f) == 0 && ok ? 0 : -1;
}

int psp_state_load(const char *path) {
    if (!path || !psp_mem.ram || !psp_mem.vram || !psp_mem.scratch) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    state_head h;
    int ok = read_all(f, &h, sizeof h);
    if (!ok || h.magic != PSP_STATE_MAGIC || h.version != PSP_STATE_VERSION ||
        h.ram_size != PSP_RAM_SIZE || h.vram_size != PSP_VRAM_SIZE ||
        h.scratch_size != PSP_SCRATCH_SIZE) {
        fclose(f);
        return -1;
    }
    ok = read_all(f, psp_mem.ram, h.ram_size) &&
         read_all(f, psp_mem.vram, h.vram_size) &&
         read_all(f, psp_mem.scratch, h.scratch_size);
    if (fclose(f) != 0 || !ok) return -1;

    memcpy(psp_cpu.r, h.r, sizeof psp_cpu.r);
    psp_cpu.r[0] = 0;                  /* $zero is hardwired, even in a state */
    psp_cpu.hi = h.hi;
    psp_cpu.lo = h.lo;
    psp_cpu.pc = h.pc;
    memcpy(psp_cpu.f, h.f, sizeof psp_cpu.f);
    psp_cpu.fcr31 = h.fcr31;
    memcpy(psp_cpu.v, h.v, sizeof psp_cpu.v);
    psp_cpu.vfpu_cc = h.vfpu_cc;
    psp_cpu.frame = NULL;              /* host frames are not portable */
    psp_cpu.unwind = 0;
    return 0;
}
