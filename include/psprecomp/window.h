/* psprecomp -- a window to play in.
 *
 * Shows the PSP display, scaled, and turns the keyboard and the first game
 * controller into sceCtrl input. It runs on a host thread of its own and
 * reads the game's front buffer on a 60 Hz timer, so it never waits on the
 * game and the game never waits on it.
 *
 *   Keyboard            Controller (XInput)     PSP
 *   arrows              d-pad                   d-pad
 *   (none)              left stick              analog stick
 *   Z                   A                       Cross
 *   X                   B                       Circle
 *   A                   X                       Square
 *   S                   Y                       Triangle
 *   Q / W               LB, LT / RB, RT         L / R
 *   Enter               Start                   Start
 *   Backspace           Back                    Select
 *   F11 (SDL2)          —                       toggle fullscreen
 *
 * Windows uses Win32 + GDI + XInput with no dependencies. Linux and other
 * non-Windows hosts use SDL2 by default when it is found at configure time,
 * with nearest-scaled display and keyboard/controller input; pass
 * -DPSPRECOMP_SDL2_WINDOW=OFF for a windowless, dependency-free core. */
#ifndef PSPRECOMP_WINDOW_H
#define PSPRECOMP_WINDOW_H

/* Open the window, `scale` times the PSP's 480x272, or with `scale` 0 as
 * large as fits the screen. Returns 0 if it opened. */
int psp_window_start(const char *title, int scale);

/* Nonzero once the user has closed the window. */
int psp_window_closed(void);

/* Close the window, wait for its host thread and release the backend. Safe to
 * call when no window was opened; after it, psp_window_start() may be called
 * again. A game host should call this before it tears down its own runtime. */
void psp_window_stop(void);

#endif
