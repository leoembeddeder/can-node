/* SPDX-License-Identifier: MIT */
/* Header check (see CMakeLists.txt in this directory): the header below compiles on its own. */
#include "platform.h"

/* A translation unit has to declare something, and this also uses the header: the reset
 * reasons are distinct process exit statuses, none of them the status of a normal exit. */
_Static_assert(((int)PLAT_RESET_REQUESTED != 0) && ((int)PLAT_RESET_WATCHDOG != 0),
               "a reset reason must differ from the exit status of a normal stop");
