# MISRA C:2012: what is checked, and the deviations

**What this is.** The portable code in `lib/` and `app/`, the part that would ship on a
microcontroller, is checked on every run of `scripts/static-analysis.sh` (and so in CI) with
the MISRA C:2012 addon of cppcheck 2.13. A finding outside the deviations below fails the
check.

**What this is not.** It is not a claim of MISRA compliance. The addon implements a subset of
the rules (it has no checks for the directives, nor for rules such as 1.3, 2.1, 8.13, 9.1,
17.5, 18.1 to 18.3 or 22.1 to 22.4), and compliance in the MISRA sense needs a compliance
matrix, a qualified tool and a review process that a portfolio project does not have. The
accurate statement is: *checked with cppcheck's MISRA addon, with the deviations listed
here.*

Two more rules are not evaluated, although the addon implements them: rule 2.5 (unused macro)
and rule 8.7 (external linkage for a function used in one translation unit only). They need
a view of the whole program, and cppcheck 2.13 does not run the addon's whole-program rules
when it is driven from a compile database, which is how this project runs it. Naming the
files on the command line instead makes it report them: constants of the public headers that
no module uses, and library functions that have a single caller. Both rules are advisory.

The host simulation code in `platform/posix` and the tests are not in the MISRA scope: they
are Linux programs built on signals, pthreads and stdio. They are still subject to cppcheck's
regular checks and to clang-tidy.

The deviations live in [config/misra-suppressions.txt](../config/misra-suppressions.txt),
which the analysis reads. This table has to match that file. A deviation is recorded when the
analysis reports a finding and deviating is the right answer to it, never in advance: every
row below has findings behind it today.

| Rule | Category | Scope | Why |
|---|---|---|---|
| 1.4 | required | project | The project is written in C11 on purpose: `<stdatomic.h>` for the lock-free ring between an interrupt and a task and for the watchdog manager's alive counters, which several tasks and interrupt handlers increment while the supervising task takes them (`lib/wdgm`), and `_Noreturn` on the reset functions. Rule 1.4 was introduced by Amendment 2, before MISRA had guidance for C11; MISRA C:2023 permits both features under rules of their own. |
| 8.9 | advisory | project | The statically allocated kernel objects and buffers of a module are declared together at file scope, so that the module's memory budget can be read in one place, rather than as `static` locals of the one function that passes them to the kernel. |
| 11.5 | advisory | `lib/ring/spsc_ring.c` | The ring stores elements of a caller-chosen type and, like `memcpy`, takes storage and elements as `void` pointers. They are converted only to `uint8_t *` for copying bytes. |
| 11.5 | advisory | `lib/flash/nor.c` | The flash device interface (`lib/flash/flash.h`) hands every implementation its own state as a `void *ctx`, so that the update code is written against one interface whatever part stands behind it. The NOR model's three adapters (`dev_read`, `dev_program`, `dev_erase`) convert it back to the `nor_t` that `nor_device()` put there; there is no other conversion in the module. |
| 11.5 | advisory | `lib/bootimg/bootimg.c` | lib/boot's validator interface (`boot_validator_t` in `lib/boot/boot.h`) hands the implementation its own state as a `void *ctx`, so that lib/boot does not depend on lib/image. The two callbacks (`validator_start`, `validator_step`) convert it back to the `bootimg_t` that `bootimg_init()` put there; there is no other conversion in the module. |
| 11.5 | advisory | `app/diag.c` | lib/isotp's callbacks (`isotp_callbacks_t` in `lib/isotp/isotp.h`) hand the caller its own state as a `void *context`, so that the transport does not depend on its user. The three callbacks of the node's diagnostic connection (`on_send_frame`, `on_indication`, `on_confirm`) convert it back to the `diag_t` that `diag_init()` put there; there is no other conversion in the module. |
| 11.5 | advisory | `app/diag_backend.c` | lib/uds's backend (`uds_backend_t` in `lib/uds/uds.h`) hands the implementation its own state as a `void *ctx`, so that the server does not depend on the node. The node's backend converts it back to the `diag_backend_t` that `diag_backend_init()` put there, in one function (`backend_of()`); there is no other conversion in the module. |
| 17.1, 21.6 | required | `lib/log/log.c` | A printf-style log call needs `<stdarg.h>` and `vsnprintf`. The logging front end is the only file allowed to use them; it formats into a fixed buffer on the caller's stack and never allocates. |

Functions in `lib/` and `app/` have a single point of exit (advisory rule 15.5 is followed, not
deviated from).

A local exception that applies to a single line is written next to that line as a
`cppcheck-suppress` comment with its reason. There are none for a MISRA rule in `lib/` or
`app/` today. `lib/uds/uds.c` has one for cppcheck's own style check `constParameterPointer`,
which is not a MISRA rule: the TesterPresent handler only reads the server, but has the
signature of every handler of the service table, so its server pointer cannot be const.
