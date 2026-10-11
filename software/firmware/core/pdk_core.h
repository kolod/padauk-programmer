/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PDK_CORE_H
#define PDK_CORE_H

/* Initialises the core. The very first step removes all programming voltages (principle III). */
void pdk_core_init(void);

/* One iteration of the main loop: transport, session, commands. */
void pdk_core_poll(void);

/* Called by the session when the host port opens or closes. */
void pdk_core_on_session_reset(void);

#endif
