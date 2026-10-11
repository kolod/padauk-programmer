/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Operation manager. In this increment only the busy state exists; operations arrive with US2. */
#ifndef PDK_OP_MANAGER_H
#define PDK_OP_MANAGER_H

#include <stdbool.h>

/* True while an operation with the chip is running. */
bool op_manager_busy(void);

#endif
