/* SPDX-License-Identifier: MIT */
/* Header check (see CMakeLists.txt in this directory): the header below compiles on its own. */
#include "plat_wdg.h"

/* A translation unit has to declare something, and this also uses the header: the range of
 * timeouts is not empty. */
_Static_assert(PLAT_WDG_TIMEOUT_MIN_MS <= PLAT_WDG_TIMEOUT_MAX_MS, "a timeout can be chosen");
