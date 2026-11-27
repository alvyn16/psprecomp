/* psprecomp — the Allegrex interpreter, used as a bring-up oracle.
 *
 * It shares the decoder (a_decode in tools/allegrexrecomp) and the runtime's
 * register file and memory with the recompiled path. That is the whole point:
 * when the oracle and generated C disagree, the bug is in exactly one of them,
 * not in two copies of the decode logic.
 *
 * The oracle is a stepper, not a game runner: it executes guest instructions
 * from memory with no HLE or display-list semantics. The integer core and the
 * branch/delay-slot rules are covered by tests/test_interp.c. Unsupported
 * encodings stop the run and are counted, never silently skipped.
 */
#ifndef PSPRECOMP_INTERP_H
#define PSPRECOMP_INTERP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t pc;         /* next instruction to execute */
    uint64_t steps;      /* instructions executed */
    uint64_t traps;      /* unsupported encodings met */
    int      stopped;    /* a trap stopped the run */
} psp_interp;

void psp_interp_init(psp_interp *ip, uint32_t entry);

/* Register an import stub address with the NID it calls in the HLE layer.
 * A call (jal/jalr) to that address runs psp_hle_call() and resumes after its
 * delay slot, exactly as the emitted import thunk does. */
void psp_interp_hle_add(uint32_t addr, uint32_t nid);
void psp_interp_hle_clear(void);

/* Execute exactly one instruction, including its delay slot when present.
 * Returns 1 if it executed, 0 if the interpreter was already stopped. */
int psp_interp_step(psp_interp *ip);

/* Step until `max_steps` is reached, a trap occurs, or `stop_pc` is visited
 * (pass 0 to ignore the break address). Returns the number of steps run. */
uint64_t psp_interp_run(psp_interp *ip, uint64_t max_steps, uint32_t stop_pc);

#ifdef __cplusplus
}
#endif

#endif
