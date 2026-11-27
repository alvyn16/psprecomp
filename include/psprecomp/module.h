/* psprecomp -- loading and starting a recompiled module.
 *
 * A recompiled program still needs its own image in guest memory (its data,
 * its constants, the tables its code reads), and the few facts the loader
 * would have supplied: where module_start is, the $gp its globals are
 * addressed from, and the C++ constructors crt0 would have run. */
#ifndef PSPRECOMP_MODULE_H
#define PSPRECOMP_MODULE_H

#include <stdint.h>

typedef struct {
    uint32_t base;          /* link address of the image */
    uint32_t image_end;     /* end of the file-backed part; .bss follows */
    uint32_t entry;         /* module_start */
    uint32_t gp;            /* from the module info */
    uint32_t ctors;         /* constructor table, 0 if none was found */
    int      nctors;
} psp_module;

/* Map `span` bytes at the module's link address, copy its PT_LOAD segment in,
 * and read entry, $gp and the constructor table. `span` covers .text, .data,
 * .bss and whatever a module linked at 0 expects above them. Needs
 * psp_mem_init() first. Returns 0, or -1 with a message on stderr. */
int psp_module_load(const char *path, uint32_t span, psp_module *m);

/* The command line the loader hands module_start, as (arglen, argp).
 *
 * The PSP kernel does not call module_start with a null argument block: it
 * passes a NUL-terminated command-line string and its length, and crt0 tokenises
 * that into main's argv. Passing (0, NULL) instead leaves argc at zero, which a
 * title is entitled to treat as a malformed launch -- Dante's Inferno checks
 * `argc != 1`, parses options, requires `-w`, and exits.
 *
 * Set this before psp_module_start to reproduce a launch. The string is copied
 * into guest memory; an empty string is a valid one-token command line
 * (argc == 1, argv[0] == ""). The default has no command line at all, which is
 * what the earlier WTF bring-up assumed. */
void psp_module_set_cmdline(const char *line);

/* Set up the registers the loader would have, run the constructors and call
 * module_start on this host thread. Needs the HLE initialised and the
 * recompiled functions registered. Returns module_start's result. */
uint32_t psp_module_start(const psp_module *m);

#endif
