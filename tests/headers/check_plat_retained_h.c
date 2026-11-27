/* SPDX-License-Identifier: MIT */
/* Header check (see CMakeLists.txt in this directory): the header below compiles on its own. */
#include "plat_retained.h"

/* A translation unit has to declare something, and this also uses the header: the hand-off
 * takes the reserved words, a trial's attempt fits its byte, and the cause of a power-on is no
 * exit status of a reset. */
_Static_assert(PLAT_RETAINED_RESERVED_WORDS == 4U, "the hand-off is four words");
_Static_assert(PLAT_HANDOFF_MAX_ATTEMPT <= 0xFFU, "an attempt fits its byte");
_Static_assert((int)PLAT_RESET_POWER_ON < (int)PLAT_RESET_REQUESTED,
               "a power-on is not a reset reason");
