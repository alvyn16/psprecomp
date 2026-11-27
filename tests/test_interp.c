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

int main(void) {
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    test_arithmetic();
    test_delay_slot_not_taken();
    test_delay_slot_taken();
    test_memory();
    test_trap();
    psp_mem_free();
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all interpreter checks passed (synthetic code, no game data)\n");
    return 0;
}
