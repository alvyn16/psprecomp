/* psprecomp — a minimal save-state UI for Linux hosts.
 *
 * A separate SDL2 window with Save / Load buttons, a slot selector and F5/F8
 * shortcuts. It calls psp_state_save()/psp_state_load() directly; the toolkit
 * does not depend on a GUI framework. State files are named
 * psprecomp-state-<slot>.bin in $PSPRECOMP_STATE_DIR (default: cwd).
 */
#ifndef PSPRECOMP_UI_H
#define PSPRECOMP_UI_H

/* Open the UI on a host thread. Returns 0 on success; without SDL2 it returns
 * -1. Stop must be called before the process tears SDL down. */
int  psp_ui_start(void);
void psp_ui_stop(void);

#endif
