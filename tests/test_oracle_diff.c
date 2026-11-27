/* Interpreter vs emitted C: run both over the same synthetic stream and diff
 * the complete guest state. No game data.
 *
 * The emitted side is produced by a_emit() -- the emitter a real game repo
 * uses -- compiled with the host compiler and linked against the same runtime
 * the interpreter uses. A mismatch is therefore a bug in exactly one of the
 * two paths, which is the point of the oracle. */

#include "psprecomp/interp.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/hle.h"
#include "psprecomp/vfpu.h"
#include "psprecomp/recomp_rt.h"

#include "analyze.h"
#include "emit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ORACLE_CC
#  define ORACLE_CC "cc"
#endif
#ifndef ORACLE_INCLUDE
#  define ORACLE_INCLUDE "."
#endif
#ifndef ORACLE_RUNTIME
#  define ORACLE_RUNTIME "libpsprecomp.a"
#endif
#ifndef ORACLE_SDL_LIBS
#  define ORACLE_SDL_LIBS ""
#endif
#ifndef ORACLE_CFLAGS
#  define ORACLE_CFLAGS ""
#endif
#ifndef ORACLE_LDFLAGS
#  define ORACLE_LDFLAGS ""
#endif

#define CODE  0x08804000u
#define STUB  0x08804100u
#define NID   0x12345678u

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

/* The HLE handler both processes register for the synthetic import. */
static void oracle_hle(void) { psp_cpu.r[PSP_REG_V0] = 42; }

/* Integer: arithmetic, a not-taken branch with its delay slot, a store, a
 * load, and a return. */
static const uint32_t INT_STREAM[] = {
    0x24020005u, 0x24030003u, 0x00431021u, 0x10400002u,
    0x24420001u, 0x3C040400u, 0x34840200u, 0xAC830000u,
    0x8C820000u, 0x03E00008u, 0x00000000u,
};

/* COP1: build 3.0 and 4.0 in GPRs, move them to $f0/$f1, add, move the
 * result back to $v0, then return. */
static const uint32_t FPU_STREAM[] = {
    0x3C024040u, 0x44820000u,          /* lui $v0,0x4040; mtc1 $v0,$f0 */
    0x3C034080u, 0x44830800u,          /* lui $v1,0x4080; mtc1 $v1,$f1 */
    0x46010080u,                       /* add.s $f2,$f0,$f1            */
    0x44021000u,                       /* mfc1 $v0,$f2                 */
    0x03E00008u, 0x00000000u,          /* jr $ra; nop                  */
};

/* VFPU: build a quad of ones, negate every source lane with vpfxs, add the
 * quad to itself, then return. Prefix state and all 128 VFPU registers are in
 * the snapshot, so a missed prefix or a wrong operand shows up. */
static const uint32_t VFPU_STREAM[] = {
    0xD0078080u,                       /* vone.q v0                    */
    0xDC0F0000u,                       /* vpfxs negate every lane      */
    0xDD0F0000u,                       /* vpfxt negate every lane      */
    0x60008080u,                       /* vadd.q v0, v0, v0            */
    0x03E00008u, 0x00000000u,
};

/* HLE: a call to the synthetic import stub, which runs the registered
 * handler (writing 42 to $v0), then returns. */
static const uint32_t HLE_STREAM[] = {
    0x0E201040u,                       /* jal STUB (0x08804100)        */
    0x00000000u,
    0x03E00008u, 0x00000000u,
};

/* Extended Allegrex semantics: signed MAX/MIN, EXT, DIV INT_MIN/-1,
 * CVT.S.W / CVT.W.S / ROUND.W.S / CEIL.W.S / FLOOR.W.S, and not-taken BEQL. */
static const uint32_t SEMANTICS_STREAM[] = {
    0x2404FFFFu,                       /* addiu $a0, $zero, -1         */
    0x24050001u,                       /* addiu $a1, $zero, 1          */
    0x0085102Cu,                       /* max   $v0, $a0, $a1          */
    0x0085182Du,                       /* min   $v1, $a0, $a1          */
    0x3C068000u,                       /* lui   $a2, 0x8000            */
    0x00C4001Au,                       /* div   $a2, $a0 (INT_MIN/-1)  */
    0x3C081234u, 0x35085678u,          /* li    $t0, 0x12345678        */
    (0x1Fu<<26)|(8u<<21)|(9u<<16)|(7u<<11)|(8u<<6), /* ext $t1, $t0, 8, 8 */
    0x44840800u,                       /* mtc1  $a0, $f1 (-1 bits)     */
    (0x11u<<26)|(0x14u<<21)|(1u<<11)|(0u<<6)|0x20u, /* cvt.s.w $f0,$f1 (-1.0f) */
    (0x11u<<26)|(0x10u<<21)|(0u<<11)|(2u<<6)|0x24u, /* cvt.w.s $f2,$f0 */
    0x3C0A422Au, 0x448A1800u,          /* lui $t2, 0x422A (42.5); mtc1 $t2,$f3 */
    (0x11u<<26)|(0x10u<<21)|(3u<<11)|(4u<<6)|0x0Cu, /* round.w.s $f4,$f3 */
    (0x11u<<26)|(0x10u<<21)|(3u<<11)|(5u<<6)|0x0Eu, /* ceil.w.s  $f5,$f3 */
    (0x11u<<26)|(0x10u<<21)|(3u<<11)|(6u<<6)|0x0Fu, /* floor.w.s $f6,$f3 */
    0x50850001u,                       /* beql  $a0, $a1, +1 (skipped) */
    0x240B0063u,                       /* addiu $t3, $zero, 99 (slot)  */
    0x03E00008u, 0x00000000u,          /* jr $ra; nop                  */
};

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1);
    if (s) {
        if (fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
        else s[n] = 0;
    }
    fclose(f);
    return s;
}

static void write_state(FILE *f) {
    for (int i = 0; i < PSP_NUM_GPR; i++) fprintf(f, "R%02d=%08X\n", i, psp_cpu.r[i]);
    fprintf(f, "HI=%08X LO=%08X PC=%08X FCR=%08X CC=%08X\n",
            psp_cpu.hi, psp_cpu.lo, psp_cpu.pc, psp_cpu.fcr31, psp_cpu.vfpu_cc);
    for (int i = 0; i < 32; i++) fprintf(f, "F%02d=%08X\n", i, psp_f32_to_bits(psp_cpu.f[i]));
    for (int i = 0; i < 128; i++) fprintf(f, "V%03d=%08X\n", i, psp_f32_to_bits(psp_cpu.v[i]));
    fprintf(f, "PFX=%08X/%08X/%08X\n",
            psp_mfv(0), psp_mfv(1), psp_mfv(2));
}

static void run_case(const char *name, const uint32_t *words, int n, int with_hle) {
    printf("-- %s --\n", name);
    for (int i = 0; i < n; i++) psp_write32(CODE + (uint32_t)i * 4, words[i]);

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = (const uint8_t *)psp_mem_ptr(CODE, (uint32_t)n * 4);
    an.base = CODE;
    an.size = (uint32_t)n * 4;
    an.scan_base = CODE;
    an.scan_size = (uint32_t)n * 4;
    if (with_hle) { an.stub_addr = STUB; an.stub_size = 4; }
    uint32_t seed = CODE;
    CHECK(a_discover(&an, &seed, 1) == 0, "%s: discovery runs", name);
    CHECK(an.nfuncs == 1, "%s: one function found, got %d", name, an.nfuncs);

    /* --- interpreter side --- */
    psp_cpu_reset();
    psp_vfpu_reset();
    psp_hle_register_unnamed(NID, "test", oracle_hle);
    psp_interp_hle_clear();
    if (with_hle) psp_interp_hle_add(STUB, NID);
    psp_interp ip;
    psp_interp_init(&ip, CODE);
    psp_interp_run(&ip, 100, 0);
    CHECK(ip.traps == 0 && ip.stopped, "%s: interpreter finished: traps=%llu",
          name, (unsigned long long)ip.traps);
    psp_cpu.pc = ip.pc;
    char ipath[128], epath[128];
    snprintf(ipath, sizeof ipath, "./oracle-%s-interp.txt", name);
    snprintf(epath, sizeof epath, "./oracle-%s-emitted.txt", name);
    FILE *f = fopen(ipath, "wb");
    CHECK(f != NULL, "%s: interpreter snapshot opens", name);
    if (f) { write_state(f); fclose(f); }
    char *want = slurp(ipath);
    int vl[4];
    psp_vfpu_regs(0, 4, vl);
    printf("  v0=%08X f2=%08X V0lane=%08X pfx=%08X cc=%08X\n",
           psp_cpu.r[PSP_REG_V0], psp_f32_to_bits(psp_cpu.f[2]),
           psp_f32_to_bits(psp_cpu.v[vl[0]]), psp_mfv(0), psp_cpu.vfpu_cc);

    /* --- emitted C side --- */
    psp_import_entry imports[1];
    emit_opts eo;
    memset(&eo, 0, sizeof eo);
    eo.outdir = ".";
    eo.prefix = "oracle";
    eo.module = "oracle";
    if (with_hle) { imports[0].addr = STUB; imports[0].nid = NID;
                    eo.imports = imports; eo.nimports = 1; }
    CHECK(a_emit(&an, &eo) == 0, "%s: corpus emitted", name);

    f = fopen("./oracle_main.c", "wb");
    CHECK(f != NULL, "%s: wrapper opens", name);
    if (f) {
        fprintf(f, "#include \"oracle_funcs.h\"\n"
                   "#include <psprecomp/mem.h>\n"
                   "#include <psprecomp/hle.h>\n"
                   "#include <psprecomp/vfpu.h>\n"
                   "#include <stdio.h>\n");
        if (with_hle)
            fprintf(f, "static void hle(void) { psp_cpu.r[PSP_REG_V0] = 42; }\n");
        fprintf(f,
            "int main(int argc, char **argv) {\n"
            "    (void)argc;\n"
            "    if (psp_mem_init() != 0) return 2;\n"
            "    psp_cpu_reset();\n"
            "    psp_vfpu_reset();\n"
            "    psp_hle_init();\n");
        if (with_hle)
            fprintf(f, "    psp_hle_register_unnamed(0x%08Xu, \"test\", hle);\n", NID);
        fprintf(f,
            "    psp_func_%08X();\n"
            "    FILE *f = fopen(argv[1], \"wb\");\n"
            "    if (!f) return 3;\n", CODE);
        if (with_hle)
            fprintf(f, "    psp_cpu.pc = 0x%08Xu;  /* return site of the HLE call */\n", CODE + 8u);
        fprintf(f,
            "    for (int i = 0; i < PSP_NUM_GPR; i++) fprintf(f, \"R%%02d=%%08X\\n\", i, psp_cpu.r[i]);\n"
            "    fprintf(f, \"HI=%%08X LO=%%08X PC=%%08X FCR=%%08X CC=%%08X\\n\",\n"
            "            psp_cpu.hi, psp_cpu.lo, psp_cpu.pc, psp_cpu.fcr31, psp_cpu.vfpu_cc);\n"
            "    for (int i = 0; i < 32; i++) fprintf(f, \"F%%02d=%%08X\\n\", i, psp_f32_to_bits(psp_cpu.f[i]));\n"
            "    for (int i = 0; i < 128; i++) fprintf(f, \"V%%03d=%%08X\\n\", i, psp_f32_to_bits(psp_cpu.v[i]));\n"
            "    fprintf(f, \"PFX=%%08X/%%08X/%%08X\\n\", psp_mfv(0), psp_mfv(1), psp_mfv(2));\n"
            "    fclose(f);\n    return 0;\n}\n");
        fclose(f);
    }

    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "\"%s\" -std=c11 %s -I\"%s\" -I. ./oracle_funcs_000.c ./oracle_imports.c ./oracle_main.c \"%s\" -lm %s %s -o ./oracle_body",
             ORACLE_CC, ORACLE_CFLAGS, ORACLE_INCLUDE, ORACLE_RUNTIME, ORACLE_SDL_LIBS, ORACLE_LDFLAGS);
    int rc = system(cmd);
    CHECK(rc == 0, "%s: emitted C compiles and links", name);
    if (rc == 0) {
        char cmd2[256];
        snprintf(cmd2, sizeof cmd2, "./oracle_body %s", epath);
        rc = system(cmd2);
        CHECK(rc == 0, "%s: emitted C runs", name);
        char *got = slurp(epath);
        CHECK(got && want && strcmp(got, want) == 0,
              "%s: interpreter and emitted C agree on the full guest state", name);
        if (got && want && strcmp(got, want) != 0) {
            printf("first difference:\n");
            const char *a = want, *b = got;
            while (*a && *a == *b) { a++; b++; }
            printf("interpreter: %.60s\nemitted:     %.60s\n", a, b);
        }
        free(got);
    }
    free(want);
    a_analysis_free(&an);
}

int main(void) {
#ifdef _WIN32
    printf("oracle diff harness requires a POSIX compiler invocation; skipped\n");
    return 0;
#endif
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    psp_hle_init();

    run_case("integer", INT_STREAM, 11, 0);
    run_case("fpu", FPU_STREAM, 8, 0);
    run_case("vfpu", VFPU_STREAM, 6, 0);
    run_case("hle", HLE_STREAM, 4, 1);
    run_case("semantics", SEMANTICS_STREAM, (int)(sizeof SEMANTICS_STREAM / sizeof SEMANTICS_STREAM[0]), 0);

    psp_mem_free();
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("interpreter and emitted C agree on all four streams (synthetic code, no game data)\n");
    return 0;
}
