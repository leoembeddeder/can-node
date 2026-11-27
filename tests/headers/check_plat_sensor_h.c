/* SPDX-License-Identifier: MIT */
/* Header check (see CMakeLists.txt in this directory): the header below compiles on its own. */
#include "plat_sensor.h"

/* A translation unit has to declare something, and this also uses the header: the invalid
 * masks of the read functions are uint8_t, so each group has at most 8 channels. */
_Static_assert((BMS_CELLS <= 8U) && (BMS_TEMPS <= 8U), "invalid masks are 8 bits wide");
