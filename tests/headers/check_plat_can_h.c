/* SPDX-License-Identifier: MIT */
/* Header check (see CMakeLists.txt in this directory): the header below compiles on its own. */
#include "plat_can.h"

/* A translation unit has to declare something, and this also uses the header: the two
 * interrupt causes are distinct bits, so that an interrupt can report both at once. */
_Static_assert((PLAT_CAN_IRQ_RX & PLAT_CAN_IRQ_EVENT) == 0U, "interrupt causes must not overlap");
