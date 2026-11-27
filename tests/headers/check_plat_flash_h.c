/* SPDX-License-Identifier: MIT */
/* Header check (see CMakeLists.txt in this directory): the header below compiles on its own. */
#include "plat_flash.h"

/* A translation unit has to declare something, and this also uses the header: two slots, and
 * the result of a successful operation is 0. */
_Static_assert(PLAT_FLASH_SLOTS == 2U, "slots A and B");
_Static_assert((int)FLASH_OK == 0, "FLASH_OK is 0");
