/* SPDX-License-Identifier: MIT */
/* Header check (see CMakeLists.txt in this directory): the header below compiles on its own. */
#include "FreeRTOSConfig.h"

/* A translation unit has to declare something, and this also uses the header: the tick rate
 * it configures is a positive number of ticks per second. */
_Static_assert((configTICK_RATE_HZ) > 0, "configTICK_RATE_HZ");
