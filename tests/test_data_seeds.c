/* The data-pointer heuristic must not turn data words into functions.
 * Synthetic image only, no game data. */

#include "analyze.h"

#include <stdio.h>
#include <string.h>

#define BASE  0x08804000u
#define TEXTS 0x40u

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

static void put32(uint8_t *img, uint32_t at, uint32_t v) {
    uint8_t *p = img + (at - BASE);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int main(void) {
    static uint8_t img[0x100];
    memset(img, 0, sizeof img);
    /* Two plausible-looking pointers in a data region: one into .text, one
     * into .data. Both decode as nop, so the shape heuristic accepts both. */
    static const uint32_t data[] = { BASE + 0x10u, BASE + 0x80u };
    memcpy(img + 0x80, data, sizeof data);

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = img;
    an.base = BASE;
    an.size = sizeof img;
    an.scan_base = BASE;
    an.scan_size = TEXTS;

    uint32_t found[8];
    int n = a_scan_data_pointers(&an, img + 0x80, sizeof data, found, 8);
    CHECK(n == 2, "both pointers pass the shape heuristic, got %d", n);

    int kept = a_keep_text_seeds(&an, found, n);
    CHECK(kept == 1, "only the .text pointer survives, got %d", kept);
    CHECK(found[0] == BASE + 0x10u, "the .text pointer is the one kept");

    /* Without a known .text extent nothing is filtered -- the caller has no
     * evidence to judge a pointer by. */
    an.scan_size = 0;
    CHECK(a_keep_text_seeds(&an, found, n) == 2, "no extent: keep both");

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all data-seed checks passed (synthetic code, no game data)\n");
    return 0;
}
