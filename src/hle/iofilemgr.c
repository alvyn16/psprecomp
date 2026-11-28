/* psprecomp — IoFileMgrForUser.
 *
 * The PSP's file API, mapped onto a host directory. Games address the UMD as
 * `disc0:/` and the Memory Stick as `ms0:/`, so those prefixes are rewritten to
 * subdirectories of a root the host chooses.
 *
 * Reads go straight into guest memory, which means a game loading assets is
 * doing the real thing -- and a texture or model that arrives byte-correct is
 * strong evidence the recompiled code around it is behaving too.
 */

#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#  include <io.h>
#else
#  include <dirent.h>
#endif

#define MAX_FILES 64
#define MAX_DIRS  16

/* Open flags, as the guest passes them. */
#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR   0x0003
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT  0x0200
#define PSP_O_TRUNC  0x0400

typedef struct {
    FILE    *f;
    int      used;
    int      pseudo;          /* a window into the disc image */
    uint64_t base;            /* byte offset of that window in the image */
    uint64_t window;          /* and its length, as the guest named it */
    uint64_t limit;           /* bytes left before the window ends */
} io_file;

typedef struct {
    int used;
#ifdef _WIN32
    intptr_t handle;
    struct _finddata_t data;
    int first;
    int done;
#else
    DIR *dir;
#endif
} io_dir;

static io_file g_file[MAX_FILES];
static io_dir  g_dir[MAX_DIRS];
static char    g_root[512];
static uint64_t g_bytes_read;
static char     g_iso[512];

#define ASYNC_MAX_FDS 128
static int64_t g_async_res[ASYNC_MAX_FDS];

void psp_io_set_iso(const char *path) {
    snprintf(g_iso, sizeof g_iso, "%s", path ? path : "");
}

/* The UMD's raw-sector API: opening disc0:/sce_lbn0x6b9fe_size0x41a gives a
 * file of `size` bytes starting at logical block `lbn`. Dante's Inferno uses
 * it for data whose location is in SECTORS.DIC, not in the filesystem tree. */
static int lbn_window(const char *guest, uint64_t *base, uint64_t *size) {
    const char *p = guest;
    if      (!strncmp(p, "disc0:", 6)) p += 6;
    else if (!strncmp(p, "umd0:",  5)) p += 5;
    else return 0;
    while (*p == '/' || *p == '\\') p++;
    if (strncmp(p, "sce_lbn0x", 9)) return 0;
    char *end;
    uint64_t lbn = strtoull(p + 9, &end, 16);
    if (!end || strncmp(end, "_size0x", 7)) return 0;
    uint64_t n = strtoull(end + 7, &end, 16);
    if (!end || *end) return 0;
    *base = lbn * 2048u;
    *size = n;
    return 1;
}

void psp_io_set_root(const char *root) {
    snprintf(g_root, sizeof g_root, "%s", root ? root : ".");
}

void psp_io_reset(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used && g_file[i].f) fclose(g_file[i].f);
        g_file[i].used = 0;
        g_file[i].f = NULL;
    }
    for (int i = 0; i < MAX_DIRS; i++) {
        if (!g_dir[i].used) continue;
#ifdef _WIN32
        if (g_dir[i].handle != -1) _findclose(g_dir[i].handle);
#else
        if (g_dir[i].dir) closedir(g_dir[i].dir);
#endif
    }
    memset(g_dir, 0, sizeof g_dir);
    memset(g_async_res, 0, sizeof g_async_res);
    g_bytes_read = 0;
    if (!g_root[0]) psp_io_set_root(".");
}

void psp_io_init(void) { g_root[0] = '\0'; psp_io_reset(); }

uint64_t psp_io_bytes_read(void) { return g_bytes_read; }

/* Rewrite a PSP path into a host path. The device prefix becomes a
 * subdirectory so a disc image and a memory-stick image can coexist under one
 * root without colliding. */
static void map_path(const char *guest, char *out, size_t cap) {
    const char *p = guest;
    const char *sub = "disc";

    if      (!strncmp(p, "disc0:", 6)) { p += 6; sub = "disc"; }
    else if (!strncmp(p, "umd0:",  5)) { p += 5; sub = "disc"; }
    else if (!strncmp(p, "ms0:",   4)) { p += 4; sub = "ms";   }
    else if (!strncmp(p, "flash0:",7)) { p += 7; sub = "flash";}
    else if (!strncmp(p, "host0:", 6)) { p += 6; sub = "host"; }

    while (*p == '/' || *p == '\\') p++;
    /* The firmware accepts both separators; Dante's Inferno builds most of
     * its resource paths with backslashes. On a POSIX host leaving them as-is
     * makes fopen look for a single filename containing backslashes and every
     * resource lookup fails. */
    int n = snprintf(out, cap, "%s/%s/", g_root, sub);
    if (n < 0 || (size_t)n >= cap) { out[cap - 1] = '\0'; return; }
    char *q = out + n;
    size_t room = cap - (size_t)n;
    for (; *p && room > 1; p++, room--) *q++ = (*p == '\\') ? '/' : *p;
    *q = '\0';
}

static void hle_Open(void) {
    char guest[512], host[1024];
    uint32_t name = psp_arg(0);
    if (!name) {
        if (getenv("PSPRECOMP_TRACE_IO"))
            fprintf(stderr, "io: open (null): invalid argument\n");
        psp_ret(0x80010016u);  /* EINVAL; address zero may still be mapped */
        return;
    }
    psp_str(name, guest, sizeof guest);
    uint32_t flags = psp_arg(1);
    /* An empty filename maps to root/disc/. POSIX fopen may open that directory
     * successfully, handing the guest a file descriptor that cannot read data. */
    if (!guest[0]) {
        if (getenv("PSPRECOMP_TRACE_IO"))
            fprintf(stderr, "io: open (empty): not found\n");
        psp_ret(0x80010002u);  /* ENOENT */
        return;
    }
    uint64_t lbn_base = 0, lbn_size = 0;
    int pseudo = g_iso[0] && lbn_window(guest, &lbn_base, &lbn_size);
    if (pseudo) {
        FILE *iso = fopen(g_iso, "rb");
        if (iso && fseeko(iso, (off_t)lbn_base, SEEK_SET) == 0) {
            for (int i = 0; i < MAX_FILES; i++) {
                if (g_file[i].used) continue;
                g_file[i].f = iso;
                g_file[i].used = 1;
                g_file[i].pseudo = 1;
                g_file[i].base = lbn_base;
                g_file[i].window = lbn_size;
                g_file[i].limit = lbn_size;
                if (getenv("PSPRECOMP_TRACE_IO"))
                    fprintf(stderr, "io: open \"%s\" -> image+0x%llX (%llu bytes): ok\n",
                            guest, (unsigned long long)lbn_base, (unsigned long long)lbn_size);
                psp_ret((uint32_t)(i + 3));
                return;
            }
            fclose(iso);
            psp_ret(0x80010018);
            return;
        }
        if (iso) fclose(iso);
        psp_ret(0x80010002u);
        return;
    }

    map_path(guest, host, sizeof host);

    const char *mode = "rb";
    if (flags & PSP_O_TRUNC)                  mode = (flags & PSP_O_RDWR) == PSP_O_RDWR ? "w+b" : "wb";
    else if (flags & PSP_O_APPEND)            mode = "ab";
    else if ((flags & PSP_O_RDWR) == PSP_O_RDWR) mode = "r+b";
    else if (flags & PSP_O_WRONLY)            mode = (flags & PSP_O_CREAT) ? "wb" : "r+b";

    FILE *f = fopen(host, mode);
    if (!f && (flags & PSP_O_CREAT)) f = fopen(host, "w+b");
    /* PSPRECOMP_TRACE_IO=1 names every open and whether it worked. A game
     * that cannot find its assets usually does not crash; it waits. */
    if (getenv("PSPRECOMP_TRACE_IO"))
        fprintf(stderr, "io: open \"%s\" -> \"%s\" flags 0x%X: %s\n",
                guest, host, flags, f ? "ok" : "not found");
    if (!f) {
        /* A failed open is normal (a game probing for a save file) and is not
         * worth a warning, but the mapped path is worth knowing when a game
         * cannot find assets it expects. */
        psp_ret(0x80010002);          /* ENOENT */
        return;
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used) continue;
        g_file[i].f = f;
        g_file[i].used = 1;
        g_file[i].pseudo = 0;
        g_file[i].base = g_file[i].window = g_file[i].limit = 0;
        psp_ret((uint32_t)(i + 3));   /* 0-2 are reserved for the std streams */
        return;
    }
    fclose(f);
    psp_ret(0x80010018);              /* too many open files */
}

/* SceIoStat is 88 guest bytes, independent of the host's struct stat layout.
 * Layout and type bits: PSPSDK src/user/pspiofilemgr_stat.h (BSD).
 * The file-backed mount has no disc-sector metadata: st_private stays zero.
 * Times use UTC and whole seconds; ctime is the host birth time on macOS and
 * the host ctime elsewhere (not a promise of portable creation-time support). */
static void stat_date(uint32_t at, time_t value) {
    struct tm date;
#ifdef _WIN32
    if (gmtime_s(&date, &value) != 0) return;
#else
    if (!gmtime_r(&value, &date)) return;
#endif
    psp_write16(at,      (uint16_t)(date.tm_year + 1900));
    psp_write16(at + 2,  (uint16_t)(date.tm_mon + 1));
    psp_write16(at + 4,  (uint16_t)date.tm_mday);
    psp_write16(at + 6,  (uint16_t)date.tm_hour);
    psp_write16(at + 8,  (uint16_t)date.tm_min);
    psp_write16(at + 10, (uint16_t)date.tm_sec);
}

static void hle_Getstat(void) {
    uint32_t name = psp_arg(0), out = psp_arg(1);
    char guest[512], host[1024];
    if (!name || !out || !psp_mem_ptr(out, 88)) {
        psp_ret(0x80010016u);  /* EINVAL; reject before any output write */
        return;
    }
    unsigned n;
    for (n = 0; n < sizeof guest; n++) {
        const char *p = (const char *)psp_mem_ptr(name + n, 1);
        if (!p) { psp_ret(0x80010016u); return; }
        guest[n] = *p;
        if (!guest[n]) break;
    }
    if (!n || n == sizeof guest) { psp_ret(0x80010016u); return; }
    uint64_t lbn_base, lbn_size;
    if (g_iso[0] && lbn_window(guest, &lbn_base, &lbn_size)) {
        for (unsigned i = 0; i < 88; i++) psp_write8(out + i, 0);
        psp_write32(out, 0x2000u | 0444u);
        psp_write32(out + 4, 0x20u);
        psp_write32(out + 8, (uint32_t)lbn_size);
        psp_write32(out + 12, (uint32_t)(lbn_size >> 32));
        psp_ret(0);
        return;
    }
    map_path(guest, host, sizeof host);
#ifdef _WIN32
    struct _stat64 st;
    int rc = _stat64(host, &st);
#else
    struct stat st;
    int rc = stat(host, &st);
#endif
    if (rc != 0) {
        uint32_t error = 0x80010005u;  /* EIO for other host failures */
        if (errno == ENOENT || errno == ENOTDIR) error = 0x80010002u;
        else if (errno == EACCES) error = 0x8001000Du;
        psp_ret(error);
        return;
    }
#ifdef _WIN32
    int directory = (st.st_mode & _S_IFMT) == _S_IFDIR;
    int regular = (st.st_mode & _S_IFMT) == _S_IFREG;
#else
    int directory = S_ISDIR(st.st_mode);
    int regular = S_ISREG(st.st_mode);
#endif
    if (!directory && !regular) { psp_ret(0x80010016u); return; }
    for (unsigned i = 0; i < 88; i++) psp_write8(out + i, 0);
    psp_write32(out, (directory ? 0x1000u : 0x2000u) | (st.st_mode & 0777u));
    psp_write32(out + 4, directory ? 0x10u : 0x20u);
    uint64_t size = (uint64_t)st.st_size;
    psp_write32(out + 8, (uint32_t)size);
    psp_write32(out + 12, (uint32_t)(size >> 32));
#ifdef __APPLE__
    stat_date(out + 16, st.st_birthtime);
#else
    stat_date(out + 16, st.st_ctime);
#endif
    stat_date(out + 32, st.st_atime);
    stat_date(out + 48, st.st_mtime);
    psp_ret(0);
}

static io_file *fd_arg(void) {
    int32_t fd = (int32_t)psp_arg(0) - 3;
    if (fd < 0 || fd >= MAX_FILES || !g_file[fd].used) return NULL;
    return &g_file[fd];
}

static void hle_Close(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(0x80020323); return; }
    fclose(h->f);
    h->f = NULL;
    h->used = 0;
    psp_ret(0);
}

static void hle_Read(void) {
    io_file *h = fd_arg();
    uint32_t dst = psp_arg(1), size = psp_arg(2);
    if (!h) { psp_ret(0x80020323); return; }
    if (!size) { psp_ret(0); return; }

    /* Read through a host buffer and then place it, so a read that straddles
     * the end of a guest region is rejected by the memory layer rather than
     * writing past it. */
    uint8_t *tmp = (uint8_t *)malloc(size);
    if (!tmp) { psp_ret(0x80020190); return; }

    size_t want = size;
    if (h->pseudo && want > h->limit) want = (size_t)h->limit;
    size_t got = want ? fread(tmp, 1, want, h->f) : 0;
    if (h->pseudo) h->limit -= got;
    if (got && psp_mem_write_block(dst, tmp, (uint32_t)got) != 0) {
        /* Fall back to byte-at-a-time so a partially mapped destination still
         * gets what fits, and the bad-access counter records the rest. */
        for (size_t i = 0; i < got; i++) psp_write8(dst + (uint32_t)i, tmp[i]);
    }
    free(tmp);
    g_bytes_read += got;
    psp_ret((uint32_t)got);
}

static void hle_Write(void) {
    io_file *h = fd_arg();
    uint32_t src = psp_arg(1), size = psp_arg(2);

    /* fd 1 and 2 are stdout/stderr: a game writing there is talking to us. */
    int32_t fd = (int32_t)psp_arg(0);
    if (fd == 1 || fd == 2) {
        for (uint32_t i = 0; i < size; i++) fputc(psp_read8(src + i), stderr);
        psp_ret(size);
        return;
    }
    if (!h) { psp_ret(0x80020323); return; }

    uint8_t *tmp = (uint8_t *)malloc(size ? size : 1);
    if (!tmp) { psp_ret(0x80020190); return; }
    for (uint32_t i = 0; i < size; i++) tmp[i] = psp_read8(src + i);
    size_t put = fwrite(tmp, 1, size, h->f);
    free(tmp);
    psp_ret((uint32_t)put);
}

/* sceIoLseek takes a 64-bit offset and returns one. Under o32 a 64-bit
 * argument is register-aligned, so it lands in $a2:$a3 rather than $a1:$a2 --
 * and the result comes back in $v0:$v1. Getting either wrong makes every seek
 * land somewhere plausible but wrong. */
static void hle_Lseek(void) {
    io_file *h = fd_arg();
    uint64_t off = (uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32);
    uint32_t whence = psp_arg(4);
    if (!h) { psp_ret(0x80020323); return; }

    int w = (whence == 1) ? SEEK_CUR : (whence == 2) ? SEEK_END : SEEK_SET;
    int64_t base = h->pseudo ? (int64_t)h->base : 0;
    int64_t end = h->pseudo ? (int64_t)(h->base + h->window) : 0;
    int64_t at;
    if (h->pseudo) {
        int64_t here = (int64_t)ftello(h->f);
        at = (w == SEEK_SET) ? base + (int64_t)off
           : (w == SEEK_CUR) ? here + (int64_t)off
                             : end + (int64_t)off;
        if (at < base || fseeko(h->f, (off_t)at, SEEK_SET) != 0) {
            psp_cpu.r[PSP_REG_V0] = 0xFFFFFFFFu;
            psp_cpu.r[PSP_REG_V1] = 0xFFFFFFFFu;
            return;
        }
    } else if (fseek(h->f, (long)(int64_t)off, w) != 0) {
        psp_cpu.r[PSP_REG_V0] = 0xFFFFFFFFu;
        psp_cpu.r[PSP_REG_V1] = 0xFFFFFFFFu;
        return;
    }

    int64_t pos = h->pseudo ? (int64_t)ftello(h->f) - base : (int64_t)ftell(h->f);
    if (pos < 0) {
        psp_cpu.r[PSP_REG_V0] = 0xFFFFFFFFu;
        psp_cpu.r[PSP_REG_V1] = 0xFFFFFFFFu;
        return;
    }
    if (h->pseudo) {
        h->limit = ((uint64_t)pos < h->window) ? (h->window - (uint64_t)pos) : 0;
    }
    uint64_t upos = (uint64_t)pos;
    psp_cpu.r[PSP_REG_V0] = (uint32_t)upos;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)(upos >> 32);
}

static void hle_Rename(void) {
    char a[512], b[512], ha[1024], hb[1024];
    psp_str(psp_arg(0), a, sizeof a);
    psp_str(psp_arg(1), b, sizeof b);
    map_path(a, ha, sizeof ha);
    map_path(b, hb, sizeof hb);
    psp_ret(rename(ha, hb) == 0 ? 0 : 0x80010002);
}

/* ---- directories --------------------------------------------------------- */

static void hle_Dopen(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);

    for (int i = 0; i < MAX_DIRS; i++) {
        if (g_dir[i].used) continue;
#ifdef _WIN32
        char pattern[1088];
        snprintf(pattern, sizeof pattern, "%s/*", host);
        g_dir[i].handle = _findfirst(pattern, &g_dir[i].data);
        if (g_dir[i].handle == -1) { psp_ret(0x80010002); return; }
        g_dir[i].first = 1;
        g_dir[i].done = 0;
#else
        g_dir[i].dir = opendir(host);
        if (!g_dir[i].dir) { psp_ret(0x80010002); return; }
#endif
        g_dir[i].used = 1;
        psp_ret((uint32_t)(i + 1));
        return;
    }
    psp_ret(0x80010018);
}

/* Fill a SceIoDirent. Only the name is populated: games use Dread to enumerate
 * save slots and asset directories by name, and a wrong stat block would be
 * worse than an empty one. */
static void hle_Dread(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    uint32_t dirent = psp_arg(1);
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }
    if (!dirent || !psp_mem_ptr(dirent, 88 + 256)) { psp_ret(0x80010016u); return; }

    const char *name = NULL;
#ifdef _WIN32
    if (g_dir[id].done) { psp_ret(0); return; }
    if (g_dir[id].first) {
        g_dir[id].first = 0;
        name = g_dir[id].data.name;
    } else if (_findnext(g_dir[id].handle, &g_dir[id].data) == 0) {
        name = g_dir[id].data.name;
    } else {
        g_dir[id].done = 1;
        psp_ret(0);
        return;
    }
#else
    struct dirent *de = readdir(g_dir[id].dir);
    if (!de) { psp_ret(0); return; }
    name = de->d_name;
#endif

    /* SceIoDirent: 88-byte SceIoStat, then char d_name[256]. */
    for (int i = 0; i < 88; i++) psp_write8(dirent + (uint32_t)i, 0);
    uint32_t at = dirent + 88;
    uint32_t len = 0;
    for (; len < 255 && name[len]; len++) psp_write8(at + len, (uint8_t)name[len]);
    psp_write8(at + len, 0);

    psp_ret(1);                       /* more entries may follow */
}

static void hle_Dclose(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }
#ifdef _WIN32
    if (g_dir[id].handle != -1) _findclose(g_dir[id].handle);
#else
    if (g_dir[id].dir) closedir(g_dir[id].dir);
#endif
    g_dir[id].used = 0;
    psp_ret(0);
}

/* ---- synchronous implementations backing the Async entry points ---------- */

static void record_async(int32_t fd, int64_t res) {
    if (fd >= 0 && fd < ASYNC_MAX_FDS) g_async_res[fd] = res;
}

static void hle_OpenAsync(void) {
    hle_Open();
    int32_t fd = (int32_t)psp_cpu.r[PSP_REG_V0];
    if (fd >= 3) record_async(fd, fd);
}

static void hle_ReadAsync(void) {
    int32_t fd = (int32_t)psp_arg(0);
    hle_Read();
    int32_t got = (int32_t)psp_cpu.r[PSP_REG_V0];
    record_async(fd, got);
    psp_ret(got < 0 ? (uint32_t)got : SCE_KERNEL_ERROR_OK);
}

static void hle_LseekAsync(void) {
    int32_t fd = (int32_t)psp_arg(0);
    hle_Lseek();
    uint64_t pos = (uint64_t)psp_cpu.r[PSP_REG_V0] |
                   ((uint64_t)psp_cpu.r[PSP_REG_V1] << 32);
    record_async(fd, (int64_t)pos);
    psp_ret((int64_t)pos < 0 ? (uint32_t)pos : SCE_KERNEL_ERROR_OK);
}

static void hle_CloseAsync(void) {
    int32_t fd = (int32_t)psp_arg(0);
    hle_Close();
    record_async(fd, (int32_t)psp_cpu.r[PSP_REG_V0]);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void write_async_res(int32_t fd, uint32_t res_ptr) {
    int64_t v = (fd >= 0 && fd < ASYNC_MAX_FDS) ? g_async_res[fd] : 0;
    if (res_ptr && psp_mem_ptr(res_ptr, 8)) {
        psp_write32(res_ptr, (uint32_t)(uint64_t)v);
        psp_write32(res_ptr + 4, (uint32_t)((uint64_t)v >> 32));
    }
}

static void hle_PollAsync(void) {
    write_async_res((int32_t)psp_arg(0), psp_arg(1));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_WaitAsyncCB(void) {
    psp_hle_call(0x349D6D6Cu); /* sceKernelCheckCallback */
    write_async_res((int32_t)psp_arg(0), psp_arg(1));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_IoOk(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

void psp_io_register(void) {
    psp_hle_register(0xACE946E8, "IoFileMgrForUser", "sceIoGetstat",     hle_Getstat);
    psp_hle_register(0x109F50BC, "IoFileMgrForUser", "sceIoOpen",        hle_Open);
    psp_hle_register(0x810C4BC3, "IoFileMgrForUser", "sceIoClose",       hle_Close);
    psp_hle_register(0x6A638D83, "IoFileMgrForUser", "sceIoRead",        hle_Read);
    psp_hle_register(0x42EC03AC, "IoFileMgrForUser", "sceIoWrite",       hle_Write);
    psp_hle_register(0x27EB27B8, "IoFileMgrForUser", "sceIoLseek",       hle_Lseek);
    psp_hle_register(0x779103A0, "IoFileMgrForUser", "sceIoRename",      hle_Rename);
    psp_hle_register(0xB29DDF9C, "IoFileMgrForUser", "sceIoDopen",       hle_Dopen);
    psp_hle_register(0xE3EB004C, "IoFileMgrForUser", "sceIoDread",       hle_Dread);
    psp_hle_register(0xEB092469, "IoFileMgrForUser", "sceIoDclose",      hle_Dclose);
    psp_hle_register(0x89AA9906, "IoFileMgrForUser", "sceIoOpenAsync",   hle_OpenAsync);
    psp_hle_register(0xA0B5A7C2, "IoFileMgrForUser", "sceIoReadAsync",   hle_ReadAsync);
    psp_hle_register(0x71B19E77, "IoFileMgrForUser", "sceIoLseekAsync",  hle_LseekAsync);
    psp_hle_register(0xFF5940B6, "IoFileMgrForUser", "sceIoCloseAsync",  hle_CloseAsync);
    psp_hle_register(0x3251EA56, "IoFileMgrForUser", "sceIoPollAsync",   hle_PollAsync);
    psp_hle_register(0xE23EEC33, "IoFileMgrForUser", "sceIoWaitAsync",   hle_PollAsync);
    psp_hle_register(0x35DBD746, "IoFileMgrForUser", "sceIoWaitAsyncCB", hle_WaitAsyncCB);
    psp_hle_register(0x54F5FB11, "IoFileMgrForUser", "sceIoDevctl",      hle_IoOk);
    psp_hle_register(0x55F4717Du, "IoFileMgrForUser", "sceIoChdir",      hle_IoOk);
    psp_hle_register(0xAB96437F, "IoFileMgrForUser", "sceIoSync",        hle_IoOk);
}
