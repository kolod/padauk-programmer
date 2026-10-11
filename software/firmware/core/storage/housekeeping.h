/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Background erase: keeps the spare journal page and the unused package slots clean, one page at a
 * time, so that no command has to wait for an erase. */
#ifndef PDK_HOUSEKEEPING_H
#define PDK_HOUSEKEEPING_H

#include <stdbool.h>

void housekeeping_init(void);

/* One step from the main loop: checks a running erase, or starts the next one. Never blocks for an
 * erase on flash that erases in the background. */
void housekeeping_poll(void);

/* Waits for the running background erase (if any) and records its result. Called before a
 * blocking erase or before writing a page that might be under erase. */
void housekeeping_finish(void);

/* True while there is nothing left to erase. */
bool housekeeping_idle(void);

#endif
