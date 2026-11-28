/* Synthetic paths and a CMake-created file, no game data. */
#include "psprecomp/hle.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, message) do { if (!(c)) { \
    printf("FAIL line %d: %s\n", __LINE__, message); failures++; } } while (0)

static uint32_t open_guest(uint32_t path) {
    psp_cpu.r[PSP_REG_A0] = path;
    psp_cpu.r[PSP_REG_A1] = 1;  /* PSP_O_RDONLY */
    psp_cpu.r[PSP_REG_A2] = 0;
    psp_hle_call(0x109F50BCu);
    return psp_cpu.r[PSP_REG_V0];
}
static void close_guest(uint32_t fd) {
    if ((int32_t)fd < 3) return;
    psp_cpu.r[PSP_REG_A0] = fd;
    psp_hle_call(0x810C4BC3u);
}
int main(void) {
    if (psp_mem_init() != 0) return 1;
    if (psp_mem_map_module(0, 4096) != 0) return 1;
    psp_cpu_reset();
    psp_hle_init();
    psp_io_set_root(PSP_TEST_IO_ROOT);
    const uint32_t PATH = 0x08850000u;
    const char name[] = "disc0:/probe.txt";
    CHECK(psp_mem_write_block(PATH, name, sizeof name) == 0, "prepare valid path");
    uint32_t fd = open_guest(PATH);
    CHECK((int32_t)fd >= 3, "the fixture file opens");
    close_guest(fd);

    /* On POSIX, fopen(root/disc/, "rb") succeeds. A missing root would hide
     * the bug by returning ENOENT before the guest-path guard is exercised. */
    psp_write8(PATH, 0);
    fd = open_guest(PATH);
    CHECK(fd == 0x80010002u, "empty path returns ENOENT, not a directory descriptor");
    close_guest(fd);

    /* PRX modules may map address zero. A null argument must still not be
     * interpreted as a filename stored in the module's first bytes. */
    CHECK(psp_mem_write_block(0, name, sizeof name) == 0, "prepare mapped address zero");
    uint64_t before = psp_mem_bad_access;
    fd = open_guest(0);
    CHECK((int32_t)fd < 0, "null path is rejected even if address zero is mapped");
    close_guest(fd);
    CHECK(psp_mem_bad_access == before, "rejecting null path needs no guest read");

    CHECK(psp_mem_write_block(PATH, name, sizeof name) == 0, "restore valid path");
    fd = open_guest(PATH);
    CHECK((int32_t)fd >= 3, "valid paths still open after failures");
    close_guest(fd);
    /* UMD raw-sector pseudo-files: the guest names a logical block and a
     * byte count, and the firmware reads that window out of the disc image.
     * Dante's Inferno uses these for resources listed in SECTORS.DIC. */
    char iso_path[512];
    snprintf(iso_path, sizeof iso_path, "%s/test.iso", PSP_TEST_IO_ROOT);
    FILE *iso = fopen(iso_path, "wb");
    CHECK(iso != NULL, "create a synthetic disc image");
    if (iso) {
        fseek(iso, 2048, SEEK_SET);            /* LBN 1 */
        fwrite("LBN-OK0123456789", 1, 16, iso);
        fclose(iso);
        psp_io_set_iso(iso_path);
        const char lbn[] = "disc0:/sce_lbn0x1_size0x10";
        CHECK(psp_mem_write_block(PATH, lbn, sizeof lbn) == 0, "prepare the LBN path");
        fd = open_guest(PATH);
        CHECK((int32_t)fd >= 3, "an LBN pseudo-file opens from the image");
        if ((int32_t)fd >= 3) {
            const uint32_t dst = 0x08860000u;
            psp_cpu.r[PSP_REG_A0] = (uint32_t)fd;
            psp_cpu.r[PSP_REG_A1] = dst;
            psp_cpu.r[PSP_REG_A2] = 6;
            psp_hle_call(0x6A638D83u);         /* sceIoRead */
            CHECK(psp_cpu.r[PSP_REG_V0] == 6, "the LBN window reads its bytes");
            CHECK(psp_read8(dst) == 'L' && psp_read32(dst) == 0x2D4E424Cu,
                  "the bytes come from the right sector");
            /* Reading to the end of the LBN window and seeking back must
             * restore the remaining-byte cap (looping Bink movies rely on this). */
            psp_cpu.r[PSP_REG_A0] = (uint32_t)fd;
            psp_cpu.r[PSP_REG_A1] = dst;
            psp_cpu.r[PSP_REG_A2] = 32;
            psp_hle_call(0x6A638D83u);         /* sceIoRead remaining 10 bytes */
            CHECK(psp_cpu.r[PSP_REG_V0] == 10, "LBN window clamps at size (16 - 6 = 10)");
            psp_cpu.r[PSP_REG_A0] = (uint32_t)fd;
            psp_cpu.r[PSP_REG_A1] = 0;
            psp_cpu.r[PSP_REG_A2] = 0;
            psp_cpu.r[PSP_REG_A3] = 0;
            psp_cpu.r[PSP_REG_T0] = 0;         /* SEEK_SET */
            psp_hle_call(0x27EB27B8u);         /* sceIoLseek back to start */
            CHECK(psp_cpu.r[PSP_REG_V0] == 0 && psp_cpu.r[PSP_REG_V1] == 0,
                  "LBN lseek back to 0 succeeds");
            psp_write32(dst, 0);
            psp_cpu.r[PSP_REG_A0] = (uint32_t)fd;
            psp_cpu.r[PSP_REG_A1] = dst;
            psp_cpu.r[PSP_REG_A2] = 6;
            psp_hle_call(0x6A638D83u);         /* sceIoRead after rewind */
            CHECK(psp_cpu.r[PSP_REG_V0] == 6 && psp_read32(dst) == 0x2D4E424Cu,
                  "LBN read succeeds after rewind to 0");
            close_guest(fd);
        }
        psp_io_set_iso(NULL);
        remove(iso_path);
    }

    psp_io_reset();
    psp_mem_free();
    printf("io_open: %d failures\n", failures);
    return failures ? 1 : 0;
}
