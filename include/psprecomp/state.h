/* psprecomp — save states for the recompiled guest.
 *
 * A state is the complete CPU-visible register file plus the three memory
 * regions (scratchpad, VRAM, main RAM). It is deliberately a snapshot of the
 * hardware, not of the host: guest call frames/dispatch state are rebuilt by
 * the runtime, and HLE subsystems keep their own counters, so a state is for
 * rewinding a single-threaded guest, not for snapshotting a live thread pool.
 *
 * Files start with a magic and version; a mismatch is rejected rather than
 * interpreted as stale state.
 */
#ifndef PSPRECOMP_STATE_H
#define PSPRECOMP_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSP_STATE_MAGIC   0x50535354u   /* "PSST" */
#define PSP_STATE_VERSION 1

int psp_state_save(const char *path);
int psp_state_load(const char *path);

#ifdef __cplusplus
}
#endif

#endif
