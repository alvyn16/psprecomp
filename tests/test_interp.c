/* The oracle's integer core, delay slots and memory access.
 * Synthetic instruction streams only, no game data. */

#include "psprecomp/interp.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"

#include <stdio.h>
#include <string.h>

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

static uint32_t reg_v0(void) { return psp_cpu.r[PSP_REG_V0]; }

static void put(uint32_t at, uint32_t w) {
    psp_write32(at, w);
}

static void load(const uint32_t *words, int n) {
    for (int i = 0; i < n; i++) put(CODE + (uint32_t)i * 4, words[i]);
}

static void test_arithmetic(void) {
    static const uint32_t words[] = {
        0x24020005u,   /* addiu $v0, $zero, 5  */
        0x24030003u,   /* addiu $v1, $zero, 3  */
        0x00431021u,   /* addu  $v0, $v0, $v1  */
        0x00021080u,   /* sll   $v0, $v0, 2    */
        0x03E00008u,   /* jr    $ra            */
        0x00000000u,   /* nop                  */
    };
    psp_cpu_reset();
    load(words, 6);
    psp_interp ip;
    psp_interp_init(&ip, CODE);
    CHECK(psp_interp_run(&ip, 20, 0) == 5, "five steps (branch includes its slot), got %llu",
          (unsigned long long)ip.steps);
    CHECK(reg_v0() == 32, "5 + 3, shifted twice, is 32: got %u", reg_v0());
    CHECK(ip.traps == 0, "no traps");
}

static void test_delay_slot_not_taken(void) {
    static const uint32_t words[] = {
        0x24020001u,   /* addiu $v0, $zero, 1        */
        0x10400004u,   /* beq   $v0, $zero, CODE+24  */
        0x24420001u,   /* addiu $v0, $v0, 1  (slot)  */
        0x24420001u,   /* addiu $v0, $v0, 1          */
        0x03E00008u,   /* jr    $ra                  */
        0x00000000u,   /* nop                        */
        0x24020063u,   /* addiu $v0, $zero, 99 (target) */
        0x00000000u,
    };
    psp_cpu_reset();
    load(words, 8);
    psp_interp ip;
    psp_interp_init(&ip, CODE);
    psp_interp_run(&ip, 20, 0);
    CHECK(reg_v0() == 3, "slot runs before the fall-through, got %u", reg_v0());
    CHECK(ip.steps == 4 && ip.traps == 0, "four steps, no trap: %llu/%llu",
          (unsigned long long)ip.steps, (unsigned long long)ip.traps);
}

static void test_delay_slot_taken(void) {
    static const uint32_t words[] = {
        0x24020000u,   /* addiu $v0, $zero, 0        */
        0x10400004u,   /* beq   $v0, $zero, CODE+24  */
        0x24420001u,   /* addiu $v0, $v0, 1  (slot)  */
        0x24420001u,   /* addiu $v0, $v0, 1 (skipped) */
        0x03E00008u,   /* jr    $ra                  */
        0x00000000u,
        0x24020063u,   /* addiu $v0, $zero, 99 (target) */
        0x00000000u,
    };
    psp_cpu_reset();
    load(words, 8);
    psp_interp ip;
    psp_interp_init(&ip, CODE);
    psp_interp_run(&ip, 4, 0);   /* branch, slot, target, next */
    CHECK(reg_v0() == 99, "taken branch reached the target after its slot, got %u", reg_v0());
}

static void test_memory(void) {
    static const uint32_t words[] = {
        0x3C040400u,   /* lui   $a0, 0x0400         */
        0x34840200u,   /* ori   $a0, $a0, 0x0200    */
        0x24031234u,   /* addiu $v1, $zero, 0x1234  */
        0xAC830000u,   /* sw    $v1, 0($a0)         */
        0x8C820000u,   /* lw    $v0, 0($a0)         */
        0x03E00008u,   /* jr    $ra                 */
        0x00000000u,
    };
    psp_cpu_reset();
    load(words, 7);
    psp_interp ip;
    psp_interp_init(&ip, CODE);
    psp_interp_run(&ip, 20, 0);
    CHECK(psp_read32(0x04000200u) == 0x1234u, "store through the runtime map");
    CHECK(reg_v0() == 0x1234u, "load back: 0x%08X", reg_v0());
}

static void test_trap(void) {
    static const uint32_t words[] = {
        0xD2008488u,   /* vf2id.q — not in the oracle's integer subset */
        0x03E00008u,
        0x00000000u,
    };
    psp_cpu_reset();
    load(words, 3);
    psp_interp ip;
    psp_interp_init(&ip, CODE);
    psp_interp_run(&ip, 20, 0);
    CHECK(ip.traps == 1, "unsupported encoding counted once, got %llu",
          (unsigned long long)ip.traps);
    CHECK(ip.stopped, "and stops the run");
}

static void test_allegrex_semantics(void) {
    /* MAX/MIN signed, DIV INT_MIN/-1, EXT size=rd+1, CVT.S.W/W.S/ROUND/TRUNC/CEIL/FLOOR,
     * BEQL not-taken slot nullification, and JALR writing rd instead of $ra. */
    const uint32_t words[] = {
        0x0085102Cu,                                                /* max $v0, $a0, $a1 */
        0x0085182Du,                                                /* min $v1, $a0, $a1 */
        0x00C7001Au,                                                /* div $a2, $a3 */
        (0x1Fu << 26) | (9u << 21) | (8u << 16) | (7u << 11) | (8u << 6) | 0x00u, /* ext $t0,$t1,8,8 */
        (0x11u << 26) | (0x14u << 21) | (2u << 11) | (0u << 6) | 0x20u, /* cvt.s.w $f0, $f2 */
        (0x11u << 26) | (0x10u << 21) | (0u << 11) | (4u << 6) | 0x24u, /* cvt.w.s $f4, $f0 */
        (0x11u << 26) | (0x10u << 21) | (1u << 11) | (6u << 6) | 0x0Cu, /* round.w.s $f6, $f1 */
        (0x11u << 26) | (0x10u << 21) | (3u << 11) | (8u << 6) | 0x0Cu, /* round.w.s $f8, $f3 */
        (0x11u << 26) | (0x10u << 21) | (5u << 11) | (10u << 6) | 0x0Eu, /* ceil.w.s $f10, $f5 */
        (0x11u << 26) | (0x10u << 21) | (5u << 11) | (12u << 6) | 0x0Fu, /* floor.w.s $f12, $f5 */
        (0x11u << 26) | (0x10u << 21) | (7u << 11) | (14u << 6) | 0x0Du, /* trunc.w.s $f14, $f7 */
        (0x14u << 26) | (10u << 21) | (11u << 16) | 2u,             /* beql $t2, $t3, +2 (not taken) */
        (0x09u << 26) | (0u << 21) | (12u << 16) | 99u,             /* addiu $t4, $zero, 99 (nullified) */
        (0u << 26) | (14u << 21) | (13u << 11) | 0x09u,             /* jalr $t5, $t6 */
        0x00000000u,                                                /* nop */
        0x03E00008u,                                                /* jr $ra (landed via jalr) */
        0x00000000u,                                                /* nop */
    };
    psp_cpu_reset();
    psp_cpu.r[PSP_REG_A0] = 0xFFFFFFFFu; /* -1 */
    psp_cpu.r[PSP_REG_A1] = 1u;
    psp_cpu.r[PSP_REG_A2] = 0x80000000u; /* INT32_MIN */
    psp_cpu.r[PSP_REG_A3] = 0xFFFFFFFFu; /* -1 */
    psp_cpu.r[9]  = 0x12345678u;         /* $t1 */
    psp_cpu.r[10] = 1u;                  /* $t2 */
    psp_cpu.r[11] = 2u;                  /* $t3 */
    psp_cpu.r[14] = CODE + 15u * 4u;     /* $t6 -> jr $ra at word 15 */
    psp_cpu.r[PSP_REG_RA] = 0xCAFEBABEu;

    uint32_t bneg5 = (uint32_t)(int32_t)-5;
    memcpy(&psp_cpu.f[2], &bneg5, 4);
    psp_cpu.f[1] = 42.5f;
    psp_cpu.f[3] = 43.5f;
    psp_cpu.f[5] = 42.25f;
    psp_cpu.f[7] = -42.75f;

    load(words, (int)(sizeof words / sizeof words[0]));
    psp_interp ip;
    psp_interp_init(&ip, CODE);
    psp_interp_run(&ip, 50, 0);

    CHECK(ip.traps == 0, "all instructions supported without trap, got %llu",
          (unsigned long long)ip.traps);
    CHECK(psp_cpu.r[PSP_REG_V0] == 1u, "max(-1, 1) signed == 1, got 0x%08X", psp_cpu.r[PSP_REG_V0]);
    CHECK(psp_cpu.r[PSP_REG_V1] == 0xFFFFFFFFu, "min(-1, 1) signed == -1, got 0x%08X", psp_cpu.r[PSP_REG_V1]);
    CHECK(psp_cpu.lo == 0x80000000u && psp_cpu.hi == 0u,
          "div(INT_MIN, -1) saturates cleanly: lo=0x%08X hi=0x%08X", psp_cpu.lo, psp_cpu.hi);
    CHECK(psp_cpu.r[8] == 0x56u, "ext(0x12345678, 8, 8) == 0x56, got 0x%08X", psp_cpu.r[8]);
    CHECK(psp_cpu.f[0] == -5.0f, "cvt.s.w(-5) == -5.0f, got %f", psp_cpu.f[0]);

    uint32_t w4 = 0, w6 = 0, w8 = 0, w10 = 0, w12 = 0, w14 = 0;
    memcpy(&w4,  &psp_cpu.f[4],  4);
    memcpy(&w6,  &psp_cpu.f[6],  4);
    memcpy(&w8,  &psp_cpu.f[8],  4);
    memcpy(&w10, &psp_cpu.f[10], 4);
    memcpy(&w12, &psp_cpu.f[12], 4);
    memcpy(&w14, &psp_cpu.f[14], 4);
    CHECK((int32_t)w4 == -5, "cvt.w.s(-5.0) == -5, got %d", (int32_t)w4);
    CHECK((int32_t)w6 == 42 && (int32_t)w8 == 44,
          "round.w.s ties to even (42.5->42, 43.5->44), got %d/%d", (int32_t)w6, (int32_t)w8);
    CHECK((int32_t)w10 == 43 && (int32_t)w12 == 42 && (int32_t)w14 == -42,
          "ceil/floor/trunc got %d/%d/%d", (int32_t)w10, (int32_t)w12, (int32_t)w14);
    CHECK(psp_cpu.r[12] == 0u, "not-taken beql nullifies delay slot ($t4=%u)", psp_cpu.r[12]);
    CHECK(psp_cpu.r[13] == CODE + 15u * 4u, "jalr writes link into rd ($t5=0x%08X)", psp_cpu.r[13]);
    CHECK(psp_cpu.r[PSP_REG_RA] == 0xCAFEBABEu, "jalr rd!=31 leaves $ra alone (0x%08X)", psp_cpu.r[PSP_REG_RA]);
}

int main(void) {
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    test_arithmetic();
    test_delay_slot_not_taken();
    test_delay_slot_taken();
    test_memory();
    test_trap();
    test_allegrex_semantics();
    psp_mem_free();
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all interpreter checks passed (synthetic code, no game data)\n");
    return 0;
}
