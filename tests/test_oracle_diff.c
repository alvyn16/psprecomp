/* Interpreter vs emitted C: run both over the same synthetic stream and diff
 * the guest register file and the stopping PC. No game data.
 *
 * The emitted side is produced by a_emit_one(), the same emitter used for a
 * real corpus, compiled with the host C compiler and linked against the same
 * runtime the interpreter uses. A mismatch is therefore a bug in exactly one
 * of the two paths, which is the point of the oracle. */

#include "psprecomp/interp.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"

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

#define CODE 0x08804000u

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

/* The stream: arithmetic, a not-taken branch with its delay slot, a store,
 * a load, and a return. Every instruction here is in both modelled subsets. */
static const uint32_t STREAM[] = {
    0x24020005u,   /* addiu $v0, $zero, 5       */
    0x24030003u,   /* addiu $v1, $zero, 3       */
    0x00431021u,   /* addu  $v0, $v0, $v1       */
    0x10400002u,   /* beq   $v0, $zero, +2      */
    0x24420001u,   /* addiu $v0, $v0, 1 (slot)  */
    0x3C040400u,   /* lui   $a0, 0x0400         */
    0x34840200u,   /* ori   $a0, $a0, 0x0200    */
    0xAC430000u,   /* sw    $v1, 0($a0)         */
    0x8C820000u,   /* lw    $v0, 0($a0)         */
    0x03E00008u,   /* jr    $ra                 */
    0x00000000u,   /* nop                       */
};

static const char *SNAP = "./oracle-snapshot.txt";

static void write_snapshot(const char *path) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "snapshot %s is writable", path);
    if (!f) return;
    for (int i = 0; i < PSP_NUM_GPR; i++)
        fprintf(f, "R%02d=%08X\n", i, psp_cpu.r[i]);
    fprintf(f, "PC=%08X\n", psp_cpu.pc);
    fclose(f);
}

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

int main(void) {
#ifdef _WIN32
    printf("oracle diff harness requires a POSIX compiler invocation; skipped\n");
    return 0;
#endif
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }

    for (size_t i = 0; i < sizeof STREAM / sizeof STREAM[0]; i++)
        psp_write32(CODE + (uint32_t)i * 4, STREAM[i]);

    a_analysis an;
    memset(&an, 0, sizeof an);
    /* a_discover fetches through an->code; point it at the mapped guest memory. */
    an.code = (const uint8_t *)psp_mem_ptr(CODE, (uint32_t)sizeof STREAM);
    CHECK(an.code != NULL, "code region is mapped");
    an.base = CODE;
    an.size = (uint32_t)sizeof STREAM;
    uint32_t seed = CODE;
    CHECK(a_discover(&an, &seed, 1) == 0, "discovery runs");
    CHECK(an.nfuncs == 1, "one function found, got %d", an.nfuncs);

    /* --- interpreter side --- */
    psp_cpu_reset();
    psp_interp ip;
    psp_interp_init(&ip, CODE);
    psp_interp_run(&ip, 100, 0);
    CHECK(ip.traps == 0 && ip.stopped, "interpreter finished cleanly: traps=%llu",
          (unsigned long long)ip.traps);
    psp_cpu.pc = ip.pc;                 /* the return address it stopped at */
    write_snapshot(SNAP);
    char *want = slurp(SNAP);

    /* --- emitted C side --- */
    /* Emit the corpus exactly as a game repo would: the shared header carries
     * the register aliases and instruments a single dumped body would not. */
    emit_opts eo;
    memset(&eo, 0, sizeof eo);
    eo.outdir = ".";
    eo.prefix = "oracle";
    eo.module = "oracle";
    CHECK(a_emit(&an, &eo) == 0, "corpus emitted");

    FILE *out = fopen("./oracle_main.c", "wb");
    CHECK(out != NULL, "emitted C wrapper opens");
    if (out) {
        fprintf(out,
            "#include \"oracle_funcs.h\"\n"
            "#include <psprecomp/mem.h>\n"
            "#include <stdio.h>\n"
            "int main(int argc, char **argv) {\n"
            "    (void)argc;\n"
            "    if (psp_mem_init() != 0) return 2;\n"
            "    psp_cpu_reset();\n"
            "    psp_func_%08X();\n"
            "    FILE *f = fopen(argv[1], \"wb\");\n"
            "    if (!f) return 3;\n"
            "    for (int i = 0; i < PSP_NUM_GPR; i++) fprintf(f, \"R%%02d=%%08X\\n\", i, psp_cpu.r[i]);\n"
            "    fprintf(f, \"PC=%%08X\\n\", psp_cpu.pc);\n"
            "    fclose(f);\n"
            "    return 0;\n"
            "}\n", CODE);
        fclose(out);
    }

    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "\"%s\" -std=c11 -I\"%s\" -I. ./oracle_funcs_000.c ./oracle_main.c \"%s\" -lm -o ./oracle_body",
             ORACLE_CC, ORACLE_INCLUDE, ORACLE_RUNTIME);
    int rc = system(cmd);
    CHECK(rc == 0, "emitted C compiles and links (%s)", cmd);
    if (rc == 0) {
        rc = system("./oracle_body ./oracle-emitted.txt");
        CHECK(rc == 0, "emitted C runs");
        char *got = slurp("./oracle-emitted.txt");
        CHECK(got && want && strcmp(got, want) == 0,
              "interpreter and emitted C agree on all registers and PC");
        if (got && want && strcmp(got, want) != 0) {
            printf("--- interpreter ---\n%s--- emitted ---\n%s", want, got);
        }
        free(got);
    }

    free(want);
    a_analysis_free(&an);
    psp_mem_free();
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("interpreter and emitted C agree (synthetic code, no game data)\n");
    return 0;
}
