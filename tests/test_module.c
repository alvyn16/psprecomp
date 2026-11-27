/* Module loading and start-up -- a synthetic ELF, no game data.
 *
 * The image is laid out the way WTF's are: module info, then code, then the
 * constructors and their null-terminated table at the end of the image. The
 * entry and constructors are host functions registered at those addresses. */

#include "psprecomp/module.h"
#include "psprecomp/mem.h"
#include "psprecomp/cpu.h"
#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

#define SEG_OFF  0x60u
#define SEG_SIZE 0x200u
#define INFO     0x40u        /* module info, in the segment */
#define ENTRY    0x100u
#define CTOR_A   0x180u
#define CTOR_B   0x184u
#define TABLE    0x1F0u
#define GP       0x00408000u

static char order[8];
static int norder;
static uint32_t start_gp, start_k0;
static uint32_t start_arglen, start_argp;
static char     start_arg[16];

static void ctor_a(void) { order[norder++] = 'a'; }
static void ctor_b(void) { order[norder++] = 'b'; }
static void start(void) {
    order[norder++] = 's';
    start_gp = psp_cpu.r[PSP_REG_GP];
    start_k0 = psp_cpu.r[26];
    start_arglen = psp_cpu.r[PSP_REG_A0];
    start_argp = psp_cpu.r[PSP_REG_A1];
    memset(start_arg, 0, sizeof start_arg);
    if (start_argp && start_arglen)
        psp_mem_read_block(start_arg, start_argp,
                           start_arglen < sizeof start_arg ? start_arglen : sizeof start_arg);
    psp_cpu.r[PSP_REG_V0] = 0;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int main(void) {
    uint8_t f[SEG_OFF + SEG_SIZE];
    memset(f, 0, sizeof f);
    memcpy(f, "\x7F" "ELF", 4);
    put32(f + 24, ENTRY);                             /* e_entry */
    put32(f + 28, 52);                                /* e_phoff */
    uint8_t *ph = f + 52;
    put32(ph + 0, 1);                                 /* PT_LOAD */
    put32(ph + 4, SEG_OFF);
    put32(ph + 8, 0);                                 /* linked at 0, as WTF is */
    put32(ph + 12, 0x80000000u | (SEG_OFF + INFO));   /* p_paddr: module info */
    put32(ph + 16, SEG_SIZE);
    put32(ph + 20, SEG_SIZE);
    put32(f + SEG_OFF + INFO + 32, GP);
    put32(f + SEG_OFF + TABLE, CTOR_A);
    put32(f + SEG_OFF + TABLE + 4, CTOR_B);
    put32(f + SEG_OFF + TABLE + 8, 0);

    const char *path = "test_module.elf";
    FILE *o = fopen(path, "wb");
    if (!o || fwrite(f, 1, sizeof f, o) != sizeof f) { printf("cannot write %s\n", path); return 1; }
    fclose(o);

    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    psp_cpu_reset();
    psp_hle_init();

    psp_module m;
    CHECK(psp_module_load(path, 0x10000u, &m) == 0, "the module loads");
    CHECK(m.entry == ENTRY, "entry from the ELF header: 0x%08X", m.entry);
    CHECK(m.gp == GP, "gp from the module info: 0x%08X", m.gp);
    CHECK(m.ctors == TABLE && m.nctors == 2, "constructor table 0x%08X, %d entries", m.ctors, m.nctors);
    CHECK(psp_read32(TABLE + 4) == CTOR_B, "the image is in guest memory");

    psp_register(ENTRY, start);
    psp_register(CTOR_A, ctor_a);
    psp_register(CTOR_B, ctor_b);
    psp_module_start(&m);
    order[norder] = 0;
    CHECK(strcmp(order, "abs") == 0, "constructors in order, then module_start: \"%s\"", order);
    CHECK(start_gp == GP, "module_start runs with the module's gp: 0x%08X", start_gp);
    CHECK(start_k0 && psp_read32(start_k0 + 4), "k0 points at a thread block with a reent pointer");
    CHECK(start_arglen == 0 && start_argp == 0,
          "no command line by default (arglen=%u argp=0x%08X)", start_arglen, start_argp);

    /* The kernel calls module_start with (arglen, argp) for a command line, not
     * with nothing: crt0 turns that string into main's argc/argv. Dante's
     * Inferno reads argc, so an empty argument block is a different program. */
    norder = 0;
    psp_module_set_cmdline("EBOOT.BIN");
    psp_module_start(&m);
    CHECK(start_arglen == (uint32_t)strlen("EBOOT.BIN") + 1,
          "arglen counts the NUL, got %u", start_arglen);
    CHECK(strcmp(start_arg, "EBOOT.BIN") == 0,
          "the command line is in guest memory, got \"%s\"", start_arg);
    norder = 0;
    psp_module_set_cmdline("");
    psp_module_start(&m);
    CHECK(start_arglen == 1 && start_arg[0] == '\0',
          "an empty command line is still one argument (arglen=%u)", start_arglen);
    norder = 0;
    psp_module_set_cmdline(NULL);
    psp_module_start(&m);
    CHECK(start_arglen == 0 && start_argp == 0, "clearing the command line restores the default");

    remove(path);

    /* An ET_EXEC (e_type == 2) must not scan the tail of its image as a PRX
     * constructor table, because .data pointer arrays look identical. */
    f[16] = 2;
    f[17] = 0;
    o = fopen(path, "wb");
    if (!o || fwrite(f, 1, sizeof f, o) != sizeof f) { printf("cannot write %s\n", path); return 1; }
    fclose(o);
    psp_module exec_m;
    CHECK(psp_module_load(path, 0x10000u, &exec_m) == 0, "ET_EXEC module loads");
    CHECK(exec_m.ctors == 0 && exec_m.nctors == 0,
          "ET_EXEC skips PRX ctor scan (got %d ctors at 0x%08X)", exec_m.nctors, exec_m.ctors);
    remove(path);

    psp_mem_free();
    printf(failures ? "module: %d failure(s)\n" : "module: all checks passed\n", failures);
    return failures ? 1 : 0;
}
