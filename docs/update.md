# The fail-safe A/B update

How the node's firmware is replaced without ever being left with nothing to start: a NOR flash
with two image slots and the metadata that say which one runs, an image format that the
bootloader can check, boot control that changes the metadata with writes a loss of supply can
interrupt at any point, a bootloader that starts the right image and hands over to it, and the
node's side: it confirms a new image once it has run in good health, and reports a rollback.
Phase 4 built all of it on the host and tested it without a transport: an update is written
into the flash by a tool that makes exactly the calls of a UDS download. Phase 5 added the
transport: a tester on the bus makes the same calls through the node's UDS server, with the
node running ([the download over the bus](#the-download-over-the-bus), and
[diagnostics.md](diagnostics.md)).

```
  fcn_flash stage, or a tester's UDS download (phase 5: eraseMemory, RequestDownload,
  TransferData, RequestTransferExit, checkProgrammingDependencies): the slot writer's calls,
  begin, erase steps, writes, finish steps
        │
        v                       the flash (lib/flash/flash.h), written through lib/boot
  ┌──────────────────────────────────────────────────────────────────┐
  │ metadata: a log of records │ slot A: an image │ slot B: an image │
  └──────────────────────────────────────────────────────────────────┘
        ^                  │                               ^
        │ select: attempt, │ the chosen slot's payload     │ the CONFIRM record
        │ rollback, ...    v                               │
  fcn_boot, the bootloader ──> hand-off in the retained cell ──> the node: its flash task
                                                                 confirms a trial after 5 s
                                                                 in good health; BMS_NodeInfo,
                                                                 BMS_BootInfo, UPDATE_ROLLBACK
```

| Part | Where | What |
|---|---|---|
| The flash device interface | [lib/flash/flash.h](../lib/flash/flash.h) | read, program, erase; the rules of a NOR flash, write-once units |
| The NOR model | [lib/flash/nor.h](../lib/flash/nor.h) | the interface over memory, with counts, wear, modelled time and seeded power cuts |
| The host's flash | [plat_flash.h](../platform/include/plat_flash.h), [posix_flash.c](../platform/posix/posix_flash.c) | the layout table; the model over a file; the waits; `--flash-cut`, `flash.cut`, `flash.status` |
| The image format | [lib/image](../lib/image/image.h), [lib/sha256](../lib/sha256/sha256.h) | the 256-byte header, validation in steps |
| Boot control and the slot writer | [lib/boot](../lib/boot/boot.h) | the metadata log, every state change, the calls of a download |
| The validator | [lib/bootimg](../lib/bootimg/bootimg.h) | lib/image's validation as lib/boot's callback, the image id |
| The bootloader | [platform/posix/boot_main.c](../platform/posix/boot_main.c) (`fcn_boot`) | select, hand-off, jump |
| The hand-off | [plat_retained.h](../platform/include/plat_retained.h) | four words of the retained cell |
| The node's side | [app/update.h](../app/update.h), THE UPDATE in [app/app.c](../app/app.c) | the confirm of a trial, the flash task, UPDATE_ROLLBACK |
| The flash tool | [platform/posix/flash_tool.c](../platform/posix/flash_tool.c) (`fcn_flash`) | factory, stage, info |
| Second implementations | [tools/fcn_tools/image.py](../tools/fcn_tools/image.py), [tools/fcn_tools/flash.py](../tools/fcn_tools/flash.py) | the image format and the flash file, written from this text |

Everything in `lib/` is portable C11 without an operating system, allocation or globals; what
belongs to the host (the layout, the file, the waits, the jump) is behind the platform layer, so
that phase 7's Cortex-M3 build replaces only that.

## The flash

The update code writes its images and its metadata into a NOR flash. It sees the flash only
through one small interface, [lib/flash/flash.h](../lib/flash/flash.h), so the same code runs
on three things: a model of a NOR part over a buffer ([lib/flash/nor.h](../lib/flash/nor.h)),
used by the unit tests; the host's flash, that model over a file
([plat_flash.h](../platform/include/plat_flash.h),
[posix_flash.c](../platform/posix/posix_flash.c)), used by the node, the bootloader and the
tools; and, in phase 7, the driver of a real part.

### The device interface

A device is a `flash_dev_t`: its geometry, a context pointer, and three operations, `read`,
`program` and `erase`. The geometry has four sizes in bytes:

| Size | Meaning | Host |
|---|---|---|
| `unit_size` | the program unit: a program writes whole units | 8 |
| `page_size` | the most one program writes; a program stays inside one page | 256 |
| `sector_size` | the erase unit | 4096 |
| `size` | the whole device, a multiple of `sector_size` | 2,105,344 |

The first three are powers of two, each dividing the next. The rules are those of a NOR flash,
with one made stricter:

- Erased bytes read 0xFF. An erase sets one whole sector to 0xFF.
- A program can only clear bits: the new byte is the old byte AND the byte given. It writes
  whole units inside one page.
- **Write-once.** A unit is programmed at most once between two erases of its sector. A program
  of a unit that does not read all 0xFF is refused with `FLASH_E_NOT_ERASED` and changes
  nothing, not even the other units of the call. A plain NOR part would let a written unit be
  programmed again to clear more bits; a flash with an error-correcting code per program unit
  (the internal flash of many microcontrollers, with a code per 64-bit double word) does not,
  because the check bits written the first time would no longer match. Code that follows the
  stricter rule runs on both, so no code above the interface clears one more bit of a flag by
  programming its unit again.
- Operations are synchronous: a call returns when the operation is over. How long that takes is
  the device's business. A caller that must stay responsive splits its work into operations,
  one sector erase or one page program per call.
- A loss of supply can cut an operation; the call then returns `FLASH_E_POWER_LOST`, and what
  the cut left is defined by the NOR model below.

A call checks, in this order, and the first check that fails decides the result:

| # | Check | Result | Changes |
|---|---|---|---|
| 1 | the device has its supply | `FLASH_E_POWER_LOST` | nothing |
| 2 | every byte of the call lies inside the device, and `len` is not 0 | `FLASH_E_RANGE` | nothing |
| 3 | a program is whole units within one page; an erase starts a sector | `FLASH_E_ALIGN` | nothing |
| 4 | every unit a program writes reads all 0xFF | `FLASH_E_NOT_ERASED` | nothing |

`FLASH_E_POWER_LOST` from a call that passed checks 2 to 4 means that this call was cut. The
operations are called by one task at a time and never from an interrupt handler; on the node
that is the one task that writes the flash.

### The NOR model

[lib/flash/nor.h](../lib/flash/nor.h) implements the interface over memory the caller gives,
used in place: whatever the memory holds is the content of the device. It is pure C, with no
globals, no allocation and no clock, and the state is the caller's `nor_t`.

**What it counts.** Every call is counted once: as unpowered, as refused (checks 2 to 4), or as
a read, a program or an erase. It also counts the bytes read, the units programmed, the cuts
that fired and the modelled time of all calls, and keeps an erase count per sector, the wear, in
an array of the caller's (a torn erase counts: its cells went through part of an erase).

**What it says about time.** The model takes no time itself; it says how long each operation
would take on the modelled part, from two figures the caller gives: a program takes one page
program time, whatever its length, an erase one sector erase time, and a read, a refused call or
an unpowered call takes none. A torn operation takes the time up to its cut.

**Power cuts.** `nor_arm_cut(nor, k, position, seed)` arms a cut of the k-th program or erase
from now. Only programs and erases that pass their checks are counted. A read changes nothing
in the array, so a cut during a read leaves exactly what a cut at the start of the next program
or erase leaves when it changes none of its bits, which is one of the outcomes the model already
gives; leaving reads out lets a campaign spend no trial on a cut that cannot leave a new state.
The operation that is cut returns `FLASH_E_POWER_LOST` and leaves:

- **a program** of n units, positions 0 to n − 1, one per unit: the units before the cut
  position written (old AND data), the unit at the position with a subset of its 1-to-0
  transitions done, and the units after it untouched;
- **an erase**, positions 0 to 15, sixteenths of its duration: a subset of the sector's 0 bits
  set to 1, from none of them to all of them, anywhere in the sector, since a sector is erased as
  a whole. Later positions leave more bits set on average.

Which subset is drawn from the seed. The model draws the cut moment, then a window of moments
over the unit's (or the erase's) duration within which its transitions happen (the cells of
one unit or sector change together, within a spread), then one moment per transition inside the
window; the transitions whose moment lies before the cut are done. A cut before the window leaves
none, after it all, and in between any subset. With `NOR_CUT_SEEDED_POSITION` the position is
drawn from the seed as well, uniformly; this is how the host arms its cuts. The draws come from
SplitMix64 (Steele, Lea and Flood, OOPSLA 2014) started from the seed, in a fixed order written
in the header, so the same seed and the same operation on the same content give the same result
on every run and every machine. No `rand()`, no clock.

After a cut the supply is gone: every call returns `FLASH_E_POWER_LOST` and changes nothing
until `nor_power_on()`. A cut fires once.

**What the model does not do.** A real part can do more than this, and a design tested only
against the model is tested against nothing of the following:

- no weak or unstable bits: a torn bit is a stable 0 or 1, while a cell left between the two
  levels can read differently from one read to the next and change on its own later;
- no retention errors and no read disturb: a bit never changes without a program or erase;
- a program tears in the order of its units. A part programs a page in an order of its own,
  often many bits at once, so a cut can leave bits of any unit of the page done. A program of one
  unit tears the same way in both; code must not rely on the order of units within one longer
  program (lib/boot does not: below);
- no erase or program suspend, and no pre-programming before an erase (some parts first program
  a sector to 0 and then erase it, and a cut early in such an erase leaves bits cleared that
  were 1);
- no wear-out: a sector works however often it is erased; the counts say how often;
- no error-correcting code: a unit reads exactly its data bits. A unit programmed with 0xFF, or
  torn before any bit changed, reads erased, and the model accepts a program of it again; on a
  part with a code per unit its check bits may already be written (lib/boot's limits, below);
- a read takes no time, and a program of part of a page takes as long as a whole page.

**The tests** ([tests/unit/test_flash.c](../tests/unit/test_flash.c)) check every rule of the
interface, the order of the checks, every counter and the wear, the time of each operation and
of a torn one, which operation a cut falls on (reads, refused calls and calls without supply
are not counted), every position of a program and of an erase on a small device and on the
host's geometry, that the same seed gives the same result and other seeds others, that across
seeds every one of the 64 subsets of six transitions occurs (in a torn unit and in a torn
sector) and nothing outside the transitions ever changes, that a later cut of an erase leaves a
superset of what an earlier one left, and that the bytes a cut leaves are those the procedure
written in nor.h gives, recomputed in the test from that text with its own SplitMix64. The unit
tests cover every line and every branch of nor.c (286 of 286 lines, 148 of 148 branches; `scripts/coverage.sh`).

### The host's flash

**The layout** is a table of the platform (`plat_flash_layout()`); the update code takes every
address from it, and the Cortex-M3 build of phase 7 has a table of its own.

| Area | Offset | Size | Sectors |
|---|---|---|---|
| metadata | 0x000000 | 0x002000 (8 KiB) | 0 and 1, used in turn by lib/boot |
| slot A | 0x002000 | 0x100000 (1 MiB) | 2 to 257 |
| slot B | 0x102000 | 0x100000 (1 MiB) | 258 to 513 |

The device is 0x202000 bytes, 2,105,344. Its geometry is that of a 3 V serial NOR, 4 KiB sectors
and 256-byte pages, with 8-byte program units so that the write-once rule is exercised at the
size a flash with a code per double word needs. The metadata lie in sectors of their own before
the slots, so that an erase of a slot never touches them. Every area starts at a sector and is
whole sectors, and an image's payload, at offset 256 of its slot, starts on a page.

A slot holds an image of the release and of the debug build of the node, stripped, but not of the
AddressSanitizer build. Unstripped, the release build fitted in phase 4 and no longer does since
phase 5 added lib/isotp, lib/uds and the diagnostic task: its debug information alone is about a
megabyte, and an image carries no symbols (a target's binary has none), so the tests pack the
stripped build:

| Build | Unstripped | Stripped | Stripped fits a slot after the 256-byte header (1,048,320 bytes) |
|---|---|---|---|
| release (RelWithDebInfo, `-O2 -g`), phase 4 | 1,011,792 | 186,768 | yes |
| release, phase 5 | 1,319,752 | 244,112 | yes, with 804,208 bytes to spare |
| debug, phase 5 | 814,736 | 326,032 | yes |
| asan, phase 5 | 3,537,232 | 2,172,064 | no |

(`stat -c %s $FCN_BUILD_ROOT/<preset>/bin/fcn_node` and `strip -o`: the phase 4 figures as that
phase measured them, the phase 5 ones on the build of 2026-11-22.)
`tests/system/test_flash_file.py` checks that the release build fits. The system tests pack
stripped images of the release build: they are smaller (less to erase and write per update),
and the asan run of `scripts/test-system.sh` starts the same images, since its own node is no
image.

**The file.** The device is a file of exactly 2,105,344 bytes, byte n of the file being byte n
of the device, named with `--flash-file PATH`. It is created erased, every byte 0xFF, when it
is missing: created exclusively, locked, and written from the start, so that a process killed
while it creates the file leaves one that is too short, never one of the right size that is not
erased. A file of any other size, or one that is not a regular file, is a start-up error and is
left as it is. The file is mapped shared (`MAP_SHARED`): every program and erase is in the
kernel's page cache as soon as it is done and is seen by the next process that maps it, also
after `_exit()` in the middle of a run, as the retained cell is. While it is open the file is
locked (`flock`), and a second process that names it fails to start: two processes driving one
flash would each change it under the other. A process releases the lock itself wherever its run
ends, and does not leave it to its exit: with the close at an orderly end, and with
`posix_flash_unlock()` directly before `_exit()` at a reset, a power cut and the shutdown
guard. The lock belongs to the file's open file description, which the mapping refers to as
well, so the kernel alone would remove it only with the process's memory map, and another
process can hold that map for a moment after the exit: the next start, made as soon as the exit
is seen, found the file "in use" now and then ([bug 012](bug-log.md#012-the-next-start-found-the-files-still-locked-by-the-run-that-had-just-ended)).
`posix_flash_unlock()` first replaces the shared mapping by a private one, so that a task still
running when the hardware watchdog's thread resets the node cannot change a file that is no
longer locked. Only a killed process (SIGKILL) leaves its lock to the kernel. The bootloader
releases its lock in the same way directly before
it starts the image, which locks the file again (the jump, below). Without `--flash-file` there
is no flash: `plat_flash_device()` returns NULL, the simulation commands answer that there is
none, and the node runs as it did before phase 4.

### Timing

Each program and erase of the host's flash waits the time the modelled part takes, the typical
figures of a 3 V serial NOR with this geometry:

| Operation | Typical (modelled) | Maximum |
|---|---|---|
| page program, tPP | 0.4 ms | 3 ms |
| 4 KiB sector erase, tSE | 45 ms | 400 ms |

Source: Winbond, W25Q128JV-DTR 3V 128M-bit serial flash memory, Revision C, March 2, 2018,
AC Electrical Characteristics. The same datasheet gives at least 100,000 program and erase
cycles per sector and more than 20 years of data retention; the model has neither wear-out nor
retention errors. The typical figures are modelled because a result should show what a part
usually does; a timeout, such as the response-pending time of the UDS server of phase 5, has to
allow the maximum. At the typical figure, erasing a whole slot takes 256 × 45 ms = 11.52 s.

How the waiting is done:

- **In the node**, the platform installs a wait hook that blocks the calling task for one tick
  (`vTaskDelay(1)`) and is called again until the modelled time has passed, as a driver of a
  serial NOR polls the busy flag of its part once per tick. The calling task is blocked while it
  waits and takes no processor time, so even the tasks below it run during an erase (on the
  node, whose flash task is the lowest, only the idle task); a program of 0.4 ms ends at the
  first tick after its time, at 1 kHz within about 1 ms. In the final runs, host times: the
  write job of a 1024-byte block, four programs, took 3.97 ms on average over SocketCAN and 3.96 ms
  over UDP ([results/isotp_throughput.json](../results/isotp_throughput.json)), and the erase job
  of a 252,560-byte image, its BEGIN record and 62 sector erases (62 × 45 ms and the record's two
  programs: 2.79 s modelled), 2.84 s in the median of 50 updates over SocketCAN
  ([results/updates.json](../results/updates.json)).
  Before the scheduler runs, the hook sleeps 1 ms instead.
- **Without a kernel** (fcn_boot and the tools), the thread sleeps with `clock_nanosleep` to an
  absolute `CLOCK_MONOTONIC` deadline.

An operation is never shorter than its modelled time; a host that runs the process late makes it
longer. The self-tests `platform_flash_waits` (without a kernel) and `platform_flash_task_wait`
(under the scheduler, with the node's hook, and a task of lower priority that has to run during
every erase) time 4 erases and 10 programs: one that ends early is a failure, one that ends more
than 20 ms (under the scheduler 22 ms) after its time is a missed timing tolerance, exit
status 75. `fcn_flash --no-wait` leaves the waits out (zero timing), for tests that prepare a
flash and do not test the waits.

### Cutting the supply

A cut is a fault of the simulated hardware, the supply that fails during a flash operation, and
needs nothing in the application:

- `--flash-cut N[:SEED]` on the command line of the node (and of fcn_boot and the flash tool,
  which parse it with the same function, `posix_flash_parse_cut()`) cuts the N-th program or
  erase of the run, N from 1 to 2,147,483,647, SEED from 0 to 4,294,967,295 (0 if left out);
- the simulation command `flash.cut N[:SEED]` cuts the N-th program or erase that starts after
  the command is served, and `flash.cut off` disarms. A cut armed later replaces one armed
  before. The client is `SimControl.flash_cut()`, `flash_cut_off()` and `flash_status()` in
  [tools/fcn_tools/simctl.py](../tools/fcn_tools/simctl.py); `flash.cut` is sent once and
  never repeated, because a second delivery after an operation would move the cut, while
  `flash.status` is asked again when an answer is lost.

The position within the operation is drawn from the seed. When the cut fires, the operation is
torn as the model says and takes the time up to its cut, a line that names the cut is written to
stderr, and the run ends as a loss of supply: in the node through
`plat_reset(PLAT_RESET_POWER_LOSS)`, which writes out the log, destroys the retained cell and
exits with status 12; without a kernel the retained cell is destroyed and the process exits with
status 12 at once. Both release the locks of the flash file and the cell directly before the
exit, so the next start can follow it at once. The next start is a power-on, and the flash file
holds what the cut left. The
line of the 8th write of the self-test's script, seed 9
(`flash_selftest task-cut flash.bin cell.bin 8:9`, release build):

```
FATAL flash: power cut in operation 8 (program of 256 bytes at 0x102f00, position 21 of 32, seed 9; 30 of 33 bits changed)
```

The torn unit was unit 21 of the page; of its 33 transitions from 1 to 0, the cut left 30 done.
The operation's number counts the programs and erases since the flash was opened, whenever the
cut was armed.

The replies of the simulation commands (platform/posix/posix_flash.h):

| Request | Reply |
|---|---|
| `flash.status` | `ok flash=on size=2105344 reads=<n> bytes_read=<n> programs=<n> units_programmed=<n> erases=<n> refused=<n> busy_ns=<ns> erases_meta=<n> erases_slot_a=<n> erases_slot_b=<n> max_sector_erases=<n> cut=none\|armed\|fired cut_in=<n> cut_seed=<n>`, or `ok flash=off` without a flash |
| `flash.cut N[:SEED]` | `ok cut=armed cut_in=<N> cut_seed=<SEED> t_ns=<CLOCK_MONOTONIC when served>` |
| `flash.cut off` | `ok cut=none t_ns=<ns>` |

The counts are those since the flash was opened; `cut_in` is the number of programs and erases
until the armed cut (1: the next) and 0 when none is armed. A fired cut ends the run, so a node
never answers `cut=fired`. Without a flash, `flash.cut` answers `err no flash in this run
(started without --flash-file)`; a malformed request answers `err` with the usage. The node's
statistics have the same figures in the member `"flash"`, with `"file"` (whether the run had
a flash) and `"created"` (whether it created the file).

**The tests.** [tests/platform/test_posix_flash.c](../tests/platform/test_posix_flash.c) runs a
fixed script of ten programs and erases over every area of the layout, with reads in between,
and cuts each of the ten operations with several seeds, two ways: from simulation control in the
test's own process, with a power-loss function that returns so that the test can see that every
later call answers `FLASH_E_POWER_LOST`; and through the path `--flash-cut` takes, in a child
process, which ends with status 12 and the FATAL line and leaves the retained cell gone. Both
times the flash file is byte for byte what lib/flash's model leaves when it runs the same script
with the same cut on a copy of the earlier content. A cut beyond the script's end never fires.
It also checks the layout table, the file (created erased, kept across a restart of the
process, a file of another size or kind refused and left alone, the lock, and the lock released
by `posix_flash_unlock()` while the file is still open and mapped, which a forked process can
then take, and after which nothing the process programs or erases reaches the file), the syntax
of a cut, every reply of both commands, and the commands served by another thread while
operations run (also under ThreadSanitizer). For
[bug 012](bug-log.md#012-the-next-start-found-the-files-still-locked-by-the-run-that-had-just-ended)
it keeps mappings of the flash file and the cell alive in a process forked for that
([map_keeper.h](../tests/platform/map_keeper.h)), past the end of a child that is cut and past
`posix_flash_close()`: another process can lock both files at once all the same. [tests/platform/flash_selftest.c](../tests/platform/flash_selftest.c)
does the cut under the FreeRTOS scheduler as well, where the run ends through `plat_reset()`
and the log line written just before the script must be in the log file; there too the parent
opens the cell and the flash at once while such a process still maps them. Its
`end-while-writing` has a task program the flash unit by unit while the run is ended from
outside the scheduler, by the hardware watchdog's thread (status 11) and by the shutdown guard
(70). The self-test's own `flock()` checks that the file's lock is removed only once the mapping
is the private one, and its own `_exit()` holds the end open after that until the task has
completed eight more programs: none of them may be in the file, and the parent opens both
files at once.
[tests/system/test_flash_file.py](../tests/system/test_flash_file.py) runs the node with and
without the option, a file of the wrong size, one another node holds, and a cut armed on the
command line and at run time.

## The image format

An image is what a tester downloads into a slot and what the bootloader starts: a header of
256 bytes at the start of the slot and the payload right after it.
[lib/image](../lib/image/image.h) encodes, decodes and validates it, with the SHA-256 of
[lib/sha256](../lib/sha256/sha256.h) and the CRC-32 of [lib/crc](../lib/crc/crc.h).
[tools/fcn_tools/image.py](../tools/fcn_tools/image.py) is a second implementation, written
from this section with nothing but Python's `struct`, `hashlib` and `zlib`, and
[tests/python/test_image.py](../tests/python/test_image.py) holds the two against each other.

```
  slot   ┌────────────────────┬──────────────────────────────┬────────────────────────┐
         │ header, 256 bytes  │ payload, payload_size bytes  │ not part of the image  │
         └────────────────────┴──────────────────────────────┴────────────────────────┘
         0                   256                  256 + payload_size             slot size
```

The payload starts at slot offset 256. On the host it is a program, a build of the node or a
test program that stands in for broken firmware, which fcn_boot runs. On the Cortex-M3 of
phase 7 it begins with the vector table, which VTOR can only point at an address aligned to a
power of two of at least 128 bytes (ARMv7-M Architecture Reference Manual, VTOR); a slot starts
on a sector boundary, so the table is 256-byte aligned. Whether 256 is enough for the number
of exceptions of the AN385 is checked in phase 7.

### The header

Every field is little-endian, least significant byte first. lib/image writes and reads the
header one byte at a time and never lays a C struct over the flash, so the layout depends
neither on the compiler's padding nor on the byte order of the machine.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | `magic` | the four ASCII bytes `F` `C` `N` `I` (0x494E4346 read as a little-endian word) |
| 4 | 2 | `header_version` | the layout of the header: 1, this table |
| 6 | 2 | `header_size` | the size of the header, which is where the payload starts: 256 |
| 8 | 4 | `target` | the build the payload is for: 1 the host (the node on the FreeRTOS POSIX port, Linux on x86-64), 2 the Cortex-M3 of the Arm MPS2 AN385 (phase 7). 0 and 0xFFFFFFFF, a zeroed and an erased word, name no target |
| 12 | 4 | `image_version` | the version of the image, encoded as below |
| 16 | 4 | `payload_size` | the length of the payload in bytes, 1 to slot size − 256 |
| 20 | 4 | `flags` | reserved for later header versions: 0 |
| 24 | 32 | `payload_sha256` | the SHA-256 (FIPS 180-4) of the `payload_size` payload bytes, the 32 digest bytes in their usual order: H0 first, each word most significant byte first, what `hashlib.sha256().digest()` returns |
| 56 | 196 | unused | 0xFF each, the value of erased flash |
| 252 | 4 | `header_crc` | CRC-32/ISO-HDLC of bytes 0 to 251: the CRC of `zlib.crc32` and of lib/crc, polynomial 0x04C11DB7 reflected, initial value and final XOR 0xFFFFFFFF |

**The version** is `major << 24 | minor << 16 | patch`, with `major` and `minor` from 0 to 255
and `patch` from 0 to 65535, written `major.minor.patch`: 1.2.3 is 0x01020003. Because the parts
go from the most to the least significant bits, two versions compare as their numbers do. The
split is that of the first 32 bits of MCUboot's image version (8-bit major, 8-bit minor, 16-bit
revision), without its 32-bit build number. Every 32-bit value is a version, and the validation
does not look at it.

### What the validation checks

The bootloader validates a slot before it starts the image in it, and the slot writer
validates the slot it has written before it commits the update. The validation reads the slot
through the flash device interface, the header first and then `payload_size` bytes of payload,
in chunks of 64 bytes, and its result is the first of these checks that fails, or `ok` if none
does:

| # | Check | Result if it fails |
|---|---|---|
| 1 | the slot lies inside the flash device and is larger than the header, and no pointer is NULL | `bad_argument` |
| 2 | every read of the flash succeeds (at any point of what follows) | `flash_error` |
| 3 | `magic` is `FCNI` | `bad_magic` |
| 4 | `header_version` is 1 | `bad_header_version` |
| 5 | `header_size` is 256 | `bad_header_size` |
| 6 | `header_crc` is the CRC-32 of bytes 0 to 251 | `bad_header_crc` |
| 7 | `flags` is 0 and every unused byte is 0xFF | `bad_reserved` |
| 8 | `target` is the target the validating build expects | `bad_target` |
| 9 | `payload_size` is at least 1 and at most slot size − 256 | `bad_payload_size` |
| 10 | the SHA-256 of the payload is `payload_sha256` | `bad_digest` |

The names are those of `image_status_name()` and of the Python tool. The whole header is read
before any check is made. Checks 3 to 5 come before the CRC on purpose: a header of a later
format, with another version and perhaps another size and another place for its CRC, is reported
as that and not as a damaged header. From check 6 on, every field that a later check relies on
is guarded by the CRC. An erased slot fails check 3. One flipped bit anywhere in the header
fails one of checks 3 to 6 (a CRC detects every single-bit error) and one anywhere in the
payload fails check 10; the unit tests flip every bit of a 556-byte image, one at a time, and
check the result of each, and the system tests flip one bit of an image in the flash file
(below).

**In steps.** A SHA-256 over a megabyte of payload takes a Cortex-M3 a long time, and the
bootloader is not the only caller: the slot writer validates in the node's flash task from
phase 5 on, and the UDS server has to answer the tester with responsePending meanwhile. So the
validation also comes in steps (`image_validate_begin`, `image_validate_step`). Each step reads
one chunk of at most 64 bytes and hashes it, which is at most two SHA-256 compressions (two only
in the step that reads the last chunk and finishes the hash). A slot whose header is accepted
takes 4 + ⌈payload_size / 64⌉ steps, one whose header is refused takes 4, and a failed read ends
the validation in the step in which it happens. `image_validate()` runs all the steps in one
call; the unit tests show that the two give the same result for the same slot, and that two
validations of two slots can run interleaved. A chunk of 64 bytes is one SHA-256 block: the
payload's chunks are aligned to its start, so each goes to the hash as one block without being
buffered, and the validator holds no more than the SHA-256 context and the header read so far
(the chunk is on the stack of the step), which fits into the RAM of a small bootloader.

**The image id.** lib/boot binds the state of a slot to an image id that its validator returns
(below). [lib/bootimg](../lib/bootimg/bootimg.h), the validator built on lib/image, gives the
SHA-256 of the 256 header bytes: the header holds the payload's digest, its size, the version and
the target, and its CRC covers them all, so two valid images that differ in any byte have
different ids, and the same payload packed under another version is another image. The id costs
four more steps of 64 bytes, so a slot that validates takes 8 + ⌈payload_size / 64⌉ steps of
lib/bootimg, each still at most one read of 64 bytes and two compressions.

### What the validation does not check

- **Authenticity.** Anybody can compute a SHA-256 and a CRC. A valid image proves that the slot
  holds, intact, what its packer wrote: integrity against a torn write, a transfer error or a
  bit that changed in the flash. It does not prove who the packer was (see [what a product would
  add](#what-a-product-would-add)).
- **Rollback.** The bootloader starts an image with an older `image_version` as readily as a
  newer one: there is no anti-rollback counter.
- **The rest of the slot.** The bytes after the payload are not read; they may hold whatever an
  earlier, longer image left there.
- **The program itself.** That the payload is a program for the target is what the packer
  states in `target`; nothing looks inside the payload.

### The tool

```bash
python -m fcn_tools.image pack --version 1.2.3 --target host PAYLOAD node.img
python -m fcn_tools.image info node.img
python -m fcn_tools.image verify node.img
python -m fcn_tools.image verify --offset 0x2000 --slot-size 0x100000 flash.bin
```

`pack` writes the header and the payload, and refuses a payload that is empty or does not fit
into the slot (`--slot-size`, default the 1 MiB of the host layout). `info` prints the header's
fields as JSON. `verify` runs the checks above on a file as the content of a slot (bytes of the
slot beyond the end of the file count as erased; with `--offset`, the slot starts at that byte
of the file, as in a flash file), prints `ok` or the name of the check that failed, and exits 0
for `ok`, 1 for a refused image and 2 for a usage error or a file it cannot read. `--target`
names the target to expect (`host`, the default, or `mps2-an385`).
[tests/tools/image_cli.c](../tests/tools/image_cli.c) is lib/image behind the same `pack` and a
`check` command, so that the tests can run thousands of images through both implementations: an
image packed by one validates in the other with the same header and step count, the C images are
byte for byte the Python ones, and about 4,800 damaged images get the same verdict on both sides.

## Boot control

[lib/boot](../lib/boot/boot.h) keeps the update metadata and makes every state change of an
update: which slot the bootloader starts, how a new image is written into the other slot, and
how it becomes the image that runs or is rolled back. It is pure C over the flash device
interface: no operating system, no allocation, no global state, and no knowledge of the image
format. The caller gives it the layout (where the metadata sectors and the two slots are) and a
validator (a callback that checks the image in a slot, in steps; on the node, in fcn_boot and in
the flash tool, lib/bootimg stands behind it). Every state change is a write to flash that a
loss of supply can cut at any point, and the design below keeps a state that decodes and boots
through every such cut, under the tear model of lib/flash: a cut program leaves its units before
the cut written, the unit at the cut with some of its 1-to-0 transitions done and the rest
untouched; a cut erase leaves some of the sector's 0 bits set to 1. Either way a cut only ever
leaves bits at 1 that the finished operation would have left at 0.

### Slots and their states

The layout has two slots, A (slot 0) and B (slot 1), of whole sectors, and 2 to 8 metadata
sectors of their own; the host table and the Cortex-M3 table of phase 7 each give theirs. The
metadata give each slot one state:

| State | Meaning | Booted? |
|---|---|---|
| EMPTY | nothing recorded: slot B in the factory state | never |
| WRITING | an update is erasing or writing the slot | never |
| TRIAL | a new image that validated when it was committed and has not confirmed itself | at most 3 times (BOOT_MAX_ATTEMPTS), each counted before the boot |
| CONFIRMED | the active image; exactly one slot is CONFIRMED in every record | every boot while no trial is pending |
| PREVIOUS | the image that was CONFIRMED before the active one | only if the active image stops validating |
| REJECTED | a trial that ran out of attempts or did not validate, or an active image that stopped validating | never again; may be overwritten by an update |

Every state but EMPTY and WRITING names the image it applies to by its 32-byte **image id**,
which the validator returns (with lib/bootimg, the SHA-256 of the image's header). An image
counts as the one a state names only if the validator accepts the slot *and* returns the
recorded id, so a slot rewritten behind the metadata's back (by a bench, a tool, or a second
update of the same slot) is never taken for the image that was confirmed there.

### The metadata log

The metadata sectors form a ring holding a log of **entries**. An entry is the record, then in
separate program units the commit field and three attempt fields; F is the field size,
max(8, program unit):

| Offset | Size | Content |
|---|---|---|
| 0 | 96, rounded up to whole units | the record (below), padded with 0xFF |
| 96 | F | the commit field |
| 96 + F | 3 × F | attempt fields 0, 1, 2 |

The stride from one entry to the next is the entry's size rounded up to a power of two when
that fits in a page, otherwise to whole pages. With the host geometry (4096-byte sectors,
256-byte pages, 8-byte units) an entry is 128 bytes, a sector holds 32 of them, and the two
metadata sectors hold 64.

**The record**, every field little-endian and written byte by byte:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic, the bytes `F` `C` `N` `B` |
| 4 | 1 | format version, 1 |
| 5 | 1 | event: what wrote the record (ADOPT 1, BEGIN 2, COMMIT 3, CONFIRM 4, ROLLBACK 5, REJECT 6, FALLBACK 7) |
| 6 | 1 | the slot the event acted on: the slot adopted, begun, committed or confirmed; the trial's for ROLLBACK and REJECT; for FALLBACK the slot fallen back to |
| 7 | 1 | reserved, 0xFF |
| 8 | 4 | sequence number, 1 to 0xFFFFFFFE: one more than the newest record before it |
| 12 | 40 | slot A: state (1 byte: EMPTY 0, WRITING 1, TRIAL 2, CONFIRMED 3, PREVIOUS 4, REJECTED 5), 3 reserved bytes 0xFF, image version (4), image id (32); EMPTY and WRITING have version 0xFFFFFFFF and an id of 0xFF bytes |
| 52 | 40 | slot B, the same |
| 92 | 4 | CRC-32/ISO-HDLC (lib/crc, equal to zlib.crc32) of bytes 0 to 91 |

**The commit field** holds the number of 0 bits in the record's bytes as a 32-bit word, then
the same word with every bit inverted, then 0xFF up to F. It is programmed in an operation of
its own, after the record's program has completed. **The attempt fields** belong to the record
of their entry: before each boot of the TRIAL image the record names, the bootloader programs
the first attempt field that reads erased, all 0x00. A field that does not read erased counts as
used, whatever it holds.

**Which record counts.** An entry holds a valid record if its commit field is exactly the one
its record bytes give, the CRC matches, and the record is well formed (magic, version, reserved
bytes, values of the enumerations, exactly one CONFIRMED slot, at most one TRIAL, no version or
id for EMPTY and WRITING). The **used part** of a metadata sector is its entries from entry 0 up
to the first entry that reads completely erased. The valid record with the highest sequence
number in the used parts is the **newest** record, and it alone is the state of the system; no
valid record at all is the **factory state**.

**Why a count of zeros.** Under the tear model a cut only leaves bits at 1 that should be 0:
in the record, in the commit field, or in both. Damage to the record lowers its number of 0
bits; damage to the commit field can only raise the number it holds. The two can never agree
again, so a record whose program was cut, a commit field whose program was cut, and an entry
damaged by a cut erase of its sector are each rejected with certainty, not with a probability.
This is a Berger code (J. M. Berger, "A note on error detection codes for asymmetric channels",
Information and Control 4, 1961), which detects every error that turns bits one way only. It
does not depend on the order in which the cells of a program change, so the model's order of
units (its stated limit) does not matter here. The CRC is there for damage outside the tear
model, where bits change both ways: an error that keeps the number of zeros (two bytes swapped)
passes the commit field and is caught by the CRC, and a unit test shows both cases.

The commit field would detect a torn record even if record and commit field were programmed in
one operation. It is still a separate, later program, as the lead's design asked: the record's
program has then completed before anything can make the record count, which matters on a part
whose interrupted program can leave cells that read 0 now and 1 later (weak bits, outside the
model).

### Writing a record

Every state change writes one record. The new record goes into the entry that ends the used
part of the newest record's sector (sector 0 in the factory state), its first erased entry:

| Step | Operation | A cut here leaves | select then sees |
|---|---|---|---|
| E | only if the used part is the whole sector: erase the next sector of the ring, which holds only records older than the newest | the newest record untouched (it is in another sector); old entries in the erased sector partly set to 1, each either intact (and older) or rejected by the commit check | the state before the call; the next record write erases that sector again |
| P1 | program the record (one program per page it spans) | an entry with part of the record and an erased commit field: not valid, but used | the state before the call; the next record goes into the entry after it |
| P2 | program the commit field (one unit) | a commit field with some of its 0 bits missing: not valid; or, if the cut came after every cell had changed, the complete record | the state before the call, or the state after it |
| R | read the log back: the new record must be the newest | the complete record | the state after the call |

No step writes into the entry of the newest record (P1 and P2 write only erased units of the
new entry, E erases a sector the newest record is not in), so the newest record survives every
cut, and after any cut the metadata decode as the state before the call or the state after it,
never as anything else. An entry is never written twice: a torn one stays in the used part and
the next record goes after it. A record that does not read back as the newest (a program that
returned OK and did nothing) is reported as BOOT_E_FLASH.

**Sequence numbers** never wrap: the record numbered 0xFFFFFFFE is the last one, and a write
after it is refused with BOOT_E_STATE, after more than 1.4 billion updates of three records
each. Two valid records with the same number are not written by any sequence of operations.

**Reading the log.** Every operation reads the metadata from the flash first (a boot_t never
works from an old copy): each used part forwards, one read per entry, up to its first erased
entry; then each used part backwards from its end until a valid record, its newest, which is
usually its last entry. Records go into a sector in the order of its entries, so in the newest
record's sector nothing that counts lies beyond the end of the used part; the other sectors hold
only older records and are erased before a record goes into them. boot_describe() decodes every
entry of the used parts instead, to count the records.

### The state machine

c is the CONFIRMED slot, o the other one; "–" means nothing is written. TRIAL(k) is a trial
whose record has k attempt fields used.

| State of o | Event | Written | New state |
|---|---|---|---|
| (factory) | select: A validates | record ADOPT, A CONFIRMED | o = B EMPTY |
| (factory) | select: A does not, B does | record ADOPT, B CONFIRMED | o = A EMPTY |
| (factory) | select: neither validates | – | factory; nothing bootable |
| any but TRIAL | select: c validates | – | unchanged; c booted |
| PREVIOUS | select: c does not validate, o does | record FALLBACK | c' = o CONFIRMED, old c REJECTED; o booted |
| EMPTY, WRITING, REJECTED, or PREVIOUS that does not validate | select: c does not validate | – | unchanged; nothing bootable |
| TRIAL(k), k < 3 | select: o validates | attempt field k | TRIAL(k + 1); o booted as attempt k + 1 |
| TRIAL(k), k < 3 | select: o does not validate | record REJECT | REJECTED; then the rows above for c |
| TRIAL(3) | select | record ROLLBACK | REJECTED; then the rows above for c |
| EMPTY, PREVIOUS, REJECTED | begin(o) | record BEGIN | WRITING |
| WRITING | begin(o) | – | WRITING |
| WRITING | finish: the image validates | record COMMIT, o TRIAL with its id | TRIAL(0) |
| WRITING | finish: it does not validate | – | WRITING |
| TRIAL(k), k ≥ 1 | confirm(o) | record CONFIRM | o CONFIRMED, old c PREVIOUS |

Refused, writing nothing (BOOT_E_STATE): begin(c); begin of either slot while o is TRIAL (a trial
is about to run or runs, and c is its fallback); begin and confirm in the factory state; confirm
of a slot that is not TRIAL; and confirm of a TRIAL that has used no attempt (it has never run,
so the caller cannot be the trial). select writes at most one record and boots at most one slot
per call. If a write of select fails without a loss of supply, select never boots the TRIAL:
an attempt that cannot be counted is not made (the trial is rolled back), and the CONFIRMED image
is booted.

A reference model of this table, written from the table and not from the code
(tests/unit/boot_test_support.c), is checked against lib/boot after every call: for every
operation in every one of ten states with three kinds of slot damage (210 cases), for 7500 random
operations on three geometries, and in every run of the power-cut campaign below.

### Each operation, and what a cut leaves

**select** (the bootloader, before the image runs) validates the slots it decides about through
the validator, then writes at most one record or one attempt field.

- Factory state: it validates A, else B, and writes ADOPT. A cut in the record write leaves the
  factory state; the next select validates and adopts the same image again.
- A TRIAL with attempts left that validates: it programs the next attempt field, then boots the
  trial. A cut before that program leaves the attempt uncounted, and the trial was not booted.
  A cut during it leaves a field with some bits at 0, which counts as used although the image
  never ran (it counts against the image, never for it), or one that reads erased, uncounted,
  and the trial was not booted. So every boot of a trial follows a completed attempt program,
  each attempt field is programmed once, and a trial is booted at most three times.
- A TRIAL with three attempts used, or one that does not validate: it writes ROLLBACK or REJECT
  and boots c. A cut in that write leaves the TRIAL as it was; the next select writes the record
  again, and still does not boot the trial.
- c does not validate and o is PREVIOUS and validates: it writes FALLBACK and boots o. A cut
  leaves c CONFIRMED; the next select does the same again.
- Otherwise it boots c and writes nothing. A select that boots the CONFIRMED image never writes
  to flash.

**confirm** (the running trial, once it has run long enough in good health) writes CONFIRM. A cut
in it leaves TRIAL(k): the next select boots the trial again as attempt k + 1, and it confirms
again. If the cut hits the confirm of the third attempt, the next select rolls the image back
although it was healthy: the limit of three boots without a confirm comes first, and the tester
repeats the update.

**begin** (the slot writer) writes BEGIN before it erases anything of o. A cut leaves o in its old
state with nothing erased, or WRITING; either way select boots c (no begin is accepted while a
trial exists, so there is no trial to boot).

**Erase steps and writes** erase and program o only. A cut leaves o WRITING with a torn sector or
page; select boots c, and the next begin of o (which writes no record, o being WRITING already)
erases every sector the image needs again.

**finish** programs the last partial page, validates the written bytes in steps, then writes
COMMIT with the image's id. A cut before or in the COMMIT write leaves o WRITING (the image is
complete but not committed; the next update starts over); a completed COMMIT makes o TRIAL(0),
and the next select boots it as attempt 1.

### The power-cut campaign of lib/boot

[tests/unit/test_boot_cuts.c](../tests/unit/test_boot_cuts.c) runs every sequence of the update
life cycle on a bench of one flash model and checks what a cut at every point leaves. The
sequences: the factory adoption (of A, and of B when A does not validate); an update of two
sectors, booted and confirmed; attempts running out and the rollback; a trial that does not
validate; begin over a rejected slot; two updates in a row; the active image breaking and the
fallback; an update that does not validate and then one that does. Each runs once without a cut,
the reference run, which numbers the programs and erases and notes the state after every call
(and is itself checked against the reference model). Then, for every operation of the reference
run, the sequence runs again from the same flash content with the supply cut just before the
operation, and torn inside it: a program at every unit position with five seeds (with the tests'
own NOR model: 1/8, 1/4, 1/2, 3/4 and all of the torn unit's transitions done), an erase with
nine seeds (nothing to all of it). After each cut the supply comes back, a fresh boot_t is made
as after a reset, and these are **decision 5's invariants**, the ones the system tests check
again from outside:

- the metadata decode, as the state before the cut call or the state after it;
- (a) select boots something (every sequence starts from a validated image that is, or becomes,
  CONFIRMED), and its choice is the reference model's for the state read;
- (b) the slot it boots validates, with the id the metadata name;
- (c) a TRIAL with attempts left that validates is booted next, as the next attempt;
- (d) select is called again and again, as after a trial that crashes every time: the trial is
  booted at most three times in all, counting the boots before the cut, and is rolled back;
- (e) the CONFIRMED image read after the cut stays CONFIRMED and is booted after the rollback;
- (f) the next update succeeds: written by the slot writer, booted as a trial, confirmed, booted.

Each sequence also runs with the log filled first by complete updates, chosen so that in some
run a record of every kind the steps write (BEGIN, COMMIT, CONFIRM, ROLLBACK, REJECT, FALLBACK)
is the one that erases the next metadata sector; the campaign checks that each such run has a
metadata erase. Three geometries: the host's (4096/256/8, two sectors of 32 entries), 1024-byte
sectors (two of 8 entries), and 512-byte sectors with 64-byte pages and 16-byte units (three
sectors of 2 entries, records over two pages). Nothing is sampled: every operation, every unit
position, every seed. The campaign is three CTest tests, each well inside the 60 s CTest gives a
unit test, and each runs twice: on the tests' own NOR model and, through the three functions of
[tests/unit/boot_test_flash.h](../tests/unit/boot_test_flash.h), on lib/flash's model, which
tears with its own seeded procedure (`*_libflash`, adapter
[tests/unit/boot_test_flash_lib.c](../tests/unit/boot_test_flash_lib.c)).

The tests print what they ran; the times are CTest's in the gate's run of
`scripts/test-unit.sh asan` (`scripts/check.sh`, load average 0.35 at its start):

| CTest test | What | Operations | Cuts | Time, the tests' model | Time, lib/flash's model (`*_libflash`) |
|---|---|---|---|---|---|
| test_boot_cuts_host | host geometry, empty log | 90 | 6,712 | 0.27 s | 0.43 s |
| test_boot_cuts_host_log | host geometry, log filled first | 176 | 14,728 | 1.69 s | 3.30 s |
| test_boot_cuts_small | the two other geometries | 342 | 11,653 | 0.48 s | 0.78 s |
| | total | 608 | 33,093 | 2.44 s | 4.51 s |

Under coverage instrumentation (`scripts/coverage.sh`, the same run) the slowest of them,
test_boot_cuts_host_log_libflash, took 16.21 s.

### Erases, programs and endurance

What each operation makes, measured by test_boot on the host geometry with a 192 KiB image
(`test_programs_and_erases_per_operation`, which prints this table):

| Operation | Programs | Units | Erases |
|---|---|---|---|
| select in the factory state: ADOPT | 2 | 13 | 0 |
| begin: BEGIN | 2 | 13 | 0 |
| erase steps | 0 | 0 | 48: ⌈image / 4096⌉ |
| writes, in 4 KiB blocks | 768: ⌈image / 256⌉ | 24,576 | 0 |
| finish: COMMIT | 2 | 13 | 0 |
| select of a trial: its attempt | 1 | 1 | 0 |
| confirm: CONFIRM | 2 | 13 | 0 |
| select of the CONFIRMED image | 0 | 0 | 0 |

A record is the program of its 12 units and the program of its commit field, and one sector
erase more when it is the first record of a metadata sector.

A complete update writes three records (BEGIN, COMMIT, CONFIRM) and one attempt field per boot
of the trial; a failed one three too (BEGIN, COMMIT, ROLLBACK or REJECT). A record erases a
metadata sector only when it is the first of a sector: with 32 entries per sector, one
metadata erase per 32 records. 100 complete updates of 192 KiB, alternating slots, made 9 metadata erases, 4800 slot erases and 77,500 programs (the same test, which asserts the 9).

Endurance, with the 100,000 program and erase cycles per sector that the W25Q128JV datasheet
gives as its minimum (the part whose timing is modelled, see [Timing](#timing); the part of
phase 7 has its own figure):

- **The slots** limit the lifetime. Updates alternate between the slots, so each sector an image
  covers is erased once every two complete updates: 200,000 complete updates.
- **The metadata**: each of the two sectors is erased once per 64 records, every 21 1/3 complete
  updates: 2.1 million complete updates, ten times the slots' figure.
- With the design the lead suggested (two sectors, one record each, erased in turn), every record
  would erase a sector: 1.5 erases per sector per update, 66,667 updates, so the metadata would
  wear out three times sooner than the slots. The log costs reading up to all of its entries at
  every operation (64 reads of 128 bytes on the host layout) and nothing in safety (the campaign
  above runs on it), and moves the limit to the slots. More metadata sectors (up to 8) lengthen
  the metadata's life in proportion.

### The slot writer

The slot writer writes a new image into the slot that is not CONFIRMED in the steps a UDS
download makes, so that the caller keeps its timing between them:

```c
boot_status_t boot_slot_by_address(const boot_t *b, uint32_t address, uint8_t *slot);
boot_status_t boot_writer_begin(boot_writer_t *w, boot_t *b, uint8_t slot, uint32_t image_size);
boot_status_t boot_writer_erase_step(boot_writer_t *w);   /* BOOT_BUSY until the last sector */
boot_status_t boot_writer_write(boot_writer_t *w, uint32_t offset, const uint8_t *data,
                                uint32_t len);
boot_status_t boot_writer_finish_step(boot_writer_t *w);  /* BOOT_BUSY until committed */
void boot_writer_progress(const boot_writer_t *w, boot_writer_progress_t *p);
```

1. **begin** checks the state (refused while a trial exists, for the CONFIRMED slot, in the
   factory state) and the size (1 byte to the slot's size), and writes BEGIN unless the slot is
   WRITING already.
2. **erase steps**: one sector per call, only the ⌈image size / sector⌉ sectors the image needs,
   each of them even if it reads erased (a sector that reads erased after a cut is not trusted).
3. **write**: any length, at consecutive offsets from 0; whole pages are programmed as they
   fill, a partial page waits in the writer (it holds one page of RAM); data past the announced
   size is refused and nothing of it is taken.
4. **finish steps**: the first programs the last partial page, padded with 0xFF to whole units,
   and starts the validation of [slot start, slot start + image size); each later call is one
   validator step (with lib/bootimg 8 + ⌈payload / 64⌉ of them, 16,388 for an image that fills
   a 1 MiB slot); the call after the image validates writes COMMIT and returns BOOT_OK. The
   image must lie within the size announced at begin: bytes left in the slot by an earlier,
   longer image are not part of it.

Before every erase and program the writer checks that the slot is still WRITING in the
metadata as its boot_t last read it, and the COMMIT record is written only after reading the
metadata again, so a state change made meanwhile through the same boot_t (another writer, a
confirm) fails the writer instead of being overwritten. An erase step is one sector erase, a
finish step at most one page program, one validator step or one record write, and a write
programs the pages its data completes.

**The tester names the slot by its address**, and it must be the start of the slot that is not
CONFIRMED; the node does not pick a slot for it. The address is what RequestDownload (and the
eraseMemory routine) carry anyway. On the Cortex-M3 of phase 7 an image executes in place, so it
is linked for one slot's address, and the tester has to send the image built for the slot it
writes: it must know which slot that is, and a node that chose the slot itself could receive an
image linked for the other one. A tester that names the running slot is refused instead of
silently writing somewhere else. The tester learns the active slot from BMS_NodeInfo (ActiveSlot)
or the flash tool. boot_slot_by_address() takes addresses of the flash device, as in the layout;
a target that maps the device into the CPU's address space subtracts its base first.

All calls on one boot_t, and on the writers that use it, come from one task: nothing locks.
Code size and stack, with arm-none-eabi-gcc `-mcpu=cortex-m3 -mthumb -Os`: lib/boot is 4794 bytes of code (boot.o 3737, boot_writer.o 1057) and no data; the largest stack frames (`-fstack-usage`) are the log's scan (448 bytes), boot_select (296), the record write (224) and the decoding of an entry (208). arm-none-eabi-gcc 13.2.1.

### Limits of boot control

- **The tear model has no weak bits.** A bit that reads 1 after a cut is taken as erased, and a
  unit that reads all 0xFF after a cut that changed none of its bits is programmed again later
  (the next record can go into such an entry, the next attempt into such a field). The model
  allows that, and so does a plain NOR part. On a part with a code per unit (ECC), an interrupted
  program can leave check bits written under data that reads 0xFF; a target driver for such a
  part must report such a unit as not erased (for example as 0x00 bytes, so that it counts as
  used) rather than as 0xFF, or lib/boot would program it again (lib/flash's nor.h says the
  same from the model's side). Retention errors, read disturb and erase suspend are outside the
  model too.
- **Nothing bootable** is a real outcome: when the active image stops validating and there is
  no PREVIOUS image that validates (the factory image alone, or the other slot WRITING or
  REJECTED), select reports it and boots nothing; a REJECTED image is never booted again, even if
  it validates.
- **A trial cannot be cancelled**: while one is committed no update can begin; it runs out of
  attempts or confirms itself.
- **A healthy trial can be rolled back** by a cut in the confirm of its third attempt (see
  confirm above).
- **The image header does not say which slot an image was linked for.** On the host that does
  not matter (images run from a copy); phase 7 has to check it before the jump (for example that
  the reset vector lies inside the slot).

## The bootloader: fcn_boot

[platform/posix/boot_main.c](../platform/posix/boot_main.c) is the bootloader of the host
build. It is a program without the FreeRTOS kernel, built from the same lib/ code as the node
(lib/flash's model behind posix_flash.c, lib/image, lib/boot, lib/bootimg).

```
fcn_boot --flash-file F [--retained-file R] [--flash-cut N[:SEED]] [--log-file L]
         [--log-level LEVEL] [-- node options...]
```

It does what a bootloader on a microcontroller does, in the same order:

1. **The retained cell.** It opens the cell with `posix_retained_open_boot()`, which reads the
   reset cause and leaves it recorded for the image. A cell that is not valid is written afresh,
   as after power-on. A hand-off an earlier start left in the cell is zeroed, as the image's
   open zeroes one (see the next section).
2. **The flash.** It opens the flash file (created erased if missing, like an empty part) and
   arms `--flash-cut` if given. Operations wait the modelled times by sleeping. A cut that fires
   destroys the cell and ends the process with status 12.
3. **The select.** It runs lib/boot's `boot_select()` with lib/bootimg behind it (the target
   IMAGE_TARGET_HOST). The select validates the slots it must decide about, counts an attempt
   before it starts a trial, and writes ADOPT, ROLLBACK, REJECT or FALLBACK when that is the
   state change to make.
4. **The copy and the command line.**
   - It decodes the chosen slot's header again and checks that its version is the one select
     validated.
   - It copies the payload into a memory file (`memfd_create("fcn-image-A"/"-B")`), hashing it
     as it copies, and compares the SHA-256 with the header, so the process starts exactly the
     bytes whose digest the header names, whatever happened to the flash since the validation.
   - It seals the file (no shrink, grow or write).
   - The image's command line is `fcn-image-<slot>`, the options after `--`, then
     `--flash-file F` and, when a cell was given, `--retained-file R`. The node options may
     not contain these two options themselves: that is a usage error.
5. **The hand-off.** Only now, when nothing but the exec is left to fail, it writes the
   hand-off into the cell's reserved words (see the next section), so that an image it does not
   start leaves no hand-off for a later start without the bootloader.
6. **The jump.** It releases its locks of the flash and the cell and calls `fexecve()`.
   - The image opens and locks both files itself. Up to the release, directly before the exec,
     fcn_boot holds both locks. The exec alone does not free them for the image in time: a
     `flock` lock belongs to the open file description, and the old process image's mappings
     refer to it as its descriptors do. The exec closes the descriptors (close-on-exec), but the
     mappings go only with the last reference to the old memory map, and another process can
     hold one past the exec: a read of `/proc/<pid>/stat` or `/proc/<pid>/maps` takes one
     ([bug 010](bug-log.md#010-the-image-found-the-files-still-locked-by-the-bootloader-it-had-replaced)).
     So fcn_boot releases both locks with `flock(LOCK_UN)` (`posix_flash_unlock()`,
     `posix_retained_unlock()`), which acts at once whatever else refers to the descriptions,
     and keeps the descriptors and the cell's mapping for a failed exec. (The flash's mapping
     is a private one from the unlock on: nothing fcn_boot stores can reach the file any more.)
   - If the exec fails, it takes the cell's lock back (`posix_retained_relock()`), then zeroes
     the hand-off's words and seals the cell again through its mapping
     (`posix_retained_clear_handoff()`; the reset reason and the rest of the cell stay as they
     were), closes the cell and ends with 42. If another process locked the cell after the
     release, the cell is that process's: fcn_boot writes nothing into it, writes
     `fcn_boot: the hand-off could not be taken back: --retained-file R is in use by another
     process` and ends with 42.
   - The process keeps its id, so the bench sees one process, the node, with the node's exit
     status. `FCN_DIE_WITH_PARENT` (the test harness's) makes the kernel kill it with its
     parent, and the setting survives the exec.

**Exit statuses**, used only when no image is started:

| Status | Meaning |
|---|---|
| 0 | `--help` or `--version` |
| 1 | a usage or start-up error |
| 12 | a power cut armed with `--flash-cut` fired in the bootloader |
| 40 | nothing bootable (`FCN_BOOT_EXIT_NOTHING_BOOTABLE`) |
| 41 | the flash could not be read (`FCN_BOOT_EXIT_FLASH`): lib/boot starts nothing from a device it cannot read |
| 42 | the chosen image could not be started (`FCN_BOOT_EXIT_JUMP`: memfd, the payload's digest, exec) |

None of these is a status of the node (0, 1, 10 to 22, 70, 74). After the jump, the exit status
is the image's. With every one of them fcn_boot has released its locks of the flash file and the
cell itself before it exits (the two closes at the end of `main()`; at 12 the two unlocks of the
power cut), so the next start finds both files free at once.

None of these ends leaves a hand-off in the cell. 0 and 1 end before the cell is opened, which
leaves it as it was, or (a flash file that cannot be used) after the open of step 1, which
zeroed an earlier hand-off. 12, 40 and 41 end in or before the select, before the hand-off is
written. 42 ends before it is written (the header read back, the copy, its digest, the seal,
the image's arguments) or after a failed exec, which takes it back. Of the bootloader's own
ends, only a kill between the write and the exec leaves one, and another program that opens
the cell between the release of its lock and the image's own open finds it: the remaining
window in the next section.

**The log.** Lines are in lib/log's format, stamped with CLOCK_MONOTONIC like the node's, and
are appended to `--log-file` (default stderr). Errors also go to stderr as `fcn_boot: ...`. A
start of a trial, from the measurements below (release build):

```
[ 12291.247372] I boot: fcn_boot 0.1.0 starting: reset cause POWER_ON (retained cell: missing, 0 resets since power-on)
[ 12291.247391] I boot: flash opened, no power cut: f1.bin
[ 12291.247398] I boot: metadata: record 3 (commit of slot B) of 3 valid, slot A confirmed 1.0.0, slot B trial 1.1.0, trial attempts used 0 of 3
[ 12291.248431] I boot: select: slot B as a trial, attempt 1 of 3, image 1.1.0
[ 12291.248898] I boot: jump: slot B at 0x102000, payload 186768 bytes, SHA-256 of the copy checked, 8 node options
```

Other lines:
- `select: slot A, the confirmed image, image 1.0.0`
- `select: adopted the image in slot A as the confirmed one (factory state)`
- `W boot: select: rolled back: the trial in slot B used its 3 attempts without confirming itself and is rejected`
- `W boot: select: rolled back: the trial in slot B does not validate and is rejected`
- `W boot: select: fell back: the confirmed image in slot B does not validate; the one before it, in slot A, is confirmed again`
- `W boot: select: its write failed (flash): the confirmed image is started`
- `metadata: factory state (no valid record in N entries used)`
- `W boot: no retained cell: the image gets no hand-off and runs as if started without a bootloader`
- `E boot: nothing bootable: slot A bad_magic, slot B bad_magic, and the metadata allow neither`

The slot verdicts are lib/image's status names. A cut in select's own write ends the process
before select has said what it decided: the log then ends with the metadata line.

**Timing** (release build, one run each, the stamps of the first log line and of the jump line): 1.14 to 1.34 ms for the confirmed image with a 186,768-byte payload (five boots), and 1.53 ms for a trial, whose attempt is one page program (0.4 ms modelled). Each validates the slot it starts and copies its payload. A select that starts the confirmed image only
reads.

**The jump on a target (phase 7).** A bootloader on the Cortex-M3 cannot start a new process; it
starts the image in place. After select, the checks below and then the hand-off, it masks
interrupts, sets VTOR to the slot's vector table (the payload, at slot offset 256), loads the
main stack pointer from the table's first word and branches to the reset handler in its second,
so that the image starts as from a reset with the bootloader's RAM given up. The image is linked
for its slot's address, and the retained cell, with the hand-off, has to be at the same address
in both linker scripts. Phase 7 builds and tests this; the host's memfd is the stand-in, and the
checks the host makes before the hand-off and `fexecve()` would have their counterparts there:
the header read back, the digest checked again (or the flash kept from writes since the
validation), and the reset vector inside the slot.

## The hand-off

The bootloader tells the image what it decided in the four reserved words of the retained cell
([plat_retained.h](../platform/include/plat_retained.h), THE HAND-OFF), sealed with the cell's
CRC:

| Word | Bits | Content |
|---|---|---|
| 0 | 0-31 | `PLAT_HANDOFF_MAGIC` 0x484E4346, the bytes "FCNH"; anything else means no hand-off |
| 1 | 0-7 | the slot started: 0 (A) or 1 (B) |
| 1 | 8-15 | 1 CONFIRMED, 2 TRIAL |
| 1 | 16-23 | the attempt: 1 to 3 for a trial, 0 for the confirmed image |
| 1 | 24-31 | the event of the select: 0 NONE, 1 ADOPT, 5 ROLLBACK, 6 REJECT, 7 FALLBACK (lib/boot's values) |
| 2 | 0-31 | the image version from the slot's header |
| 3 | 0-7 | the slot the event acted on (as in the record: the trial's for ROLLBACK and REJECT, the slot fallen back to for FALLBACK), or 0xFF (none; required for NONE) |
| 3 | 8-15 | 0, or the boot_status_t of a select write that failed |
| 3 | 16-31 | 0 |

**What the image does with it.** When the platform opens the cell, it reads the hand-off from
every cell whose magic, version, size and CRC are right, whatever its reason; after power-on
the bootloader's fresh cell records no reset. It then zeroes the four words, so that a later
start without the bootloader finds none. A hand-off that breaks a rule counts as no hand-off;
its words are zeroed too. Reserved words without the magic are kept as in phase 3.
[docs/faults.md](faults.md#the-retained-ram-cell-plat_retainedh) has the cell's other rules.

The verdict is in the statistics (`handoff.verdict`): `taken`, `invalid` or `none`. Without a
`--retained-file` there is no cell, so the image gets no hand-off and runs as if the bootloader
were not there: a trial then never confirms and is rolled back after its attempts.

The boot attempts are not counted in the cell: RAM does not survive a loss of supply, and a
trial that cuts its own supply must still run out of attempts. lib/boot counts them in the
flash, one attempt field written before each start of the trial.

**The remaining window (host only).** The bootloader writes the hand-off directly before the
exec and takes it back if the exec fails, and the image takes it when its platform opens the
cell. A hand-off therefore stays in the cell only when the run ends between the two: fcn_boot
killed after it wrote the hand-off and before the exec (or, after a failed exec, before it
zeroed the words again), or an image that ends before its platform has opened the cell. The
node and the test payloads open it as soon as they have read their command line, before any
other check of it (an image that ends at its start-up checks has taken its hand-off), so only a
command line the node cannot parse, or `--help` and `--version`, ends it before the open: the
cell's path is one of its options. A node started directly on that cell, without fcn_boot,
would take that hand-off as its own; the next start through fcn_boot zeroes it when it opens the
cell (step 1 above). On a target the bootloader runs at every reset, so there is no start
without it.

The lock of the cell is the other part of the window. fcn_boot releases it (and the flash's)
directly before the exec (step 6 above), and the image locks the cell again when it opens it.
From the release until that open, another program started on the same cell at that moment can
take it. The image then fails to start (`in use by another node`, status 1), as it would have
if that program had come first, and that program finds the hand-off: a node started directly
takes it as its own (as above), another fcn_boot zeroes it. If the exec failed, fcn_boot cannot
take the lock back, writes nothing into the cell and says that the hand-off could not be taken
back. Only two programs started on one cell at the same moment get there; a target has one
reset vector.

## The node's side of an update

[app/update.h](../app/update.h) and THE UPDATE in [app/app.c](../app/app.c).

**At start** (`app_start()`) the node reads the hand-off once; it is constant for the run. Log
lines:
- `started by the bootloader: slot B as a trial, attempt 1 of 3, image 1.1.0`
- `started by the bootloader: slot A, the confirmed image, image 1.0.0`
- `started without the bootloader (no hand-off): nothing to confirm`
- after a rollback: `W app: the bootloader rolled back before this start (ROLLBACK of slot B)`

The flash task reads the metadata once and logs
`metadata: record 3, active slot A, trial slot B with 1 of 3 attempts used`.

**On the bus** ([can-protocol.md](can-protocol.md)):
- **BMS_NodeInfo** ActiveSlot is the hand-off's slot, or SNA without the bootloader. ResetCause
  is the reset cause the bootloader left in the cell.
- **BMS_BootInfo** (0x611, once a second, in health periods 2, 12, 22, ...) carries the mode,
  the attempt, the bootloader's event and its slot, the image version from the header, the
  ConfirmState and the ConfirmSeconds. BMS_NodeInfo had no free bit for them.
- **UPDATE_ROLLBACK** (DTC 0x1B0600, P1B06-00, fault 16) is active and confirmed from the first
  BMS_Fault of a start whose hand-off reports ROLLBACK, REJECT or FALLBACK. It is judged once,
  like the reset faults, so it is reported only in the run right after the rollback
  ([faults.md](faults.md)).

**The confirm.**
- **The window.** A trial confirms itself once 50 consecutive health periods (5 s,
  `UPDATE_CONFIRM_AFTER_S`) have been in good health. A period is in good health when the
  watchdog manager's global status is OK, the CAN controller is error active (neither error
  passive nor bus off), no bus-off happened since the period before, and the controller has
  transmitted at least one frame with an acknowledgement since the period before (the transmit
  queue's count of transmit-done events grew). The CAN conditions come from the controller's own
  state and counters, not from the DTCs CAN_ERROR_PASSIVE and CAN_BUS_OFF, which can stay set
  after their fault has gone. Any other period starts the window again
  (`confirm.window_restarts`), and each condition a period failed is counted
  (`unsupervised_periods`, `not_active_periods`, `bus_off_periods`, `unacked_periods`).
- **A test hook the product does not have.** `--confirm-periods N` on the host node's command
  line makes the window N healthy periods instead of 50 (`confirm.periods_needed` reports the
  window of the run; the log says `a trial confirms itself after N healthy health periods
  (--confirm-periods)`). Phase 5's tests that leave the confirm to the tester's routine give the
  trial a window of 36,000 periods, an hour (`update_bench.TRIAL_WINDOW_PERIODS`), so that it
  cannot confirm itself before the routine: every wait of those tests ends long before. Every
  other test runs with the product's window.
- **Why 5 s, and why these conditions** (update.h, WHY 5 SECONDS). The watchdog manager finds a
  task that stops within 300 to 400 ms and the hardware watchdog resets the node 500 ms after its
  last kick, so an image that hangs or crashes in its first seconds is reset well inside the
  window, in which 50 supervision cycles go by. On the bus, a fault that other nodes signal with
  error flags, or that the node sees as bit errors, adds 8 to the transmit error counter for
  every failed frame: the controller is error passive after 16 failed frames in a row and bus off
  after 32, and either restarts the window. A node that nobody acknowledges (alone on the bus,
  cut off from it, or with a transmit path that leads nowhere) never goes bus off: under ISO
  11898-1's exception an error-passive transmitter does not count an ACK error that is all it
  sees ([lib/can/can_fc.c](../lib/can/can_fc.c), `CAN_FC_TX_ACK_ERROR_ALONE`), so its controller
  stays error passive at a transmit error counter of 128 and none of its frames is acknowledged.
  That is why error passive and a period without a newly acknowledged frame restart the window
  as well: a trial confirms itself only once it has been heard on the bus in every period of the
  window. 5 s is also short enough for system tests that run several updates. A product waits
  longer (minutes, a drive cycle) or leaves the confirm to a tester's routine after its own
  checks.
- **What the node sends in the window.** In every period it publishes ten BMS_Heartbeat (the
  sensor task) and one BMS_Fault (the health task) whatever its telemetry switch says, with
  BMS_TaskStats in every second period and BMS_NodeInfo and BMS_BootInfo once a second; with the
  telemetry on, also BMS_PackStatus every 10 ms, BMS_CellVoltages1/2 every 20 ms and
  BMS_Temperatures every 100 ms. Two reads of the count are at least 55 ms apart (the health
  task's response time is under 45 ms since phase 5's diagnostic task), so no period of a node that works sees no acknowledged
  frame, and one is all the rule asks for: the weaker "one within the last N periods" is not
  needed (update.h, WHY EVERY PERIOD NEEDS AN ACKNOWLEDGED FRAME). What the windows of
  `test_no_condition_of_a_working_node_cuts_the_window_short` counted in one run
  (`confirm.window_acked` and `confirm.fewest_acked` in the statistics, from
  `scripts/test-system.sh -k working_node`, release build, the run's
  `logs-timing/<test>/node.stats.json`; another run differs by a few frames):

  | Condition | Frames acknowledged in the window | Fewest in one period |
  |---|---|---|
  | telemetry on, nothing on the bus but a listener | 1633 | 31 |
  | telemetry switched off by the tester's TESTER_Command | 596 | 11 |
  | 125 kbit/s, under the flood of test_can_flood.py (1.2 times the bus capacity of 0x700 frames) | 1628 | 30 |

  The window's first period counts the frames acknowledged since the start. With the tester, no
  BMS_PackStatus went out after the first BMS_BootInfo (health period 2), which the test checks
  as a timing tolerance.
- **What the window does not prove.** That the trial receives: an image that cannot (a wrong
  acceptance filter, say) confirms itself all the same, as long as its own frames are
  acknowledged. A tester's routine proves the receive path: phase 5's confirm routine
  (RoutineControl 0xF000, [diagnostics.md](diagnostics.md)), whose request reaches the trial
  through that path. The routine and the self-confirm both run in the flash task, and the first
  one writes the CONFIRM record: after the self-confirm the routine is refused
  (conditionsNotCorrect, there is no trial any more) and writes nothing; after the routine the
  self-confirm writes nothing (`confirm.flash_task.self_skipped`) and the monitor is CONFIRMED by
  the routine (`confirm.flash_task.confirmed_by_routine`, log line `the trial in slot B is
  confirmed by the tester's confirm routine`).
- **The request.** The health task asks the flash task once (a task notification).
- **The flash task** (priority 1, the lowest) calls `boot_confirm()` for the hand-off's slot.
  It logs `trial in slot B ran 5 s in good health: writing its confirm` and
  `confirmed: slot B, image 1.1.0, is the active image (<n> us)`, and hands the result back. A
  failed confirm is logged as an error and is not tried again in the run: the trial then falls to
  its attempt limit, which is safe, while writing again and again into a flash that refused a
  write is not.
- **The cost.** A confirm is one record: 2 page programs (13 units) and the reads of the log.
  It took 1,697,327 ns in a run of the release build (and 1,907,570 ns in one of the system tests), most of it the two programs of 0.4 ms, each ended at the next tick by the driver's wait; the flash counted 21 reads of 2,376 bytes in all (`confirm.flash_task.confirm_ns` and `flash` in the statistics). The same record written by the tester's confirm routine took 1.43 to 2.44 ms in 59 of the 60 updates of bench/updates.py's final run at 500 kbit/s, 1.95 ms in the median (`node_trial.confirm_job.last_ns` of each record, the trial run's `jobs.confirm.last_ns`, a host time), and 47.9 ms in one, update 32 over SocketCAN, whose CONFIRM record was the flash's 97th (the factory's ADOPT and three records per update) and so the first of a metadata sector, which it erased first ([results/updates.json](../results/updates.json)).

The flash task and its place in the schedule are in
[architecture.md](architecture.md#tasks): it is the lowest-priority task, so that the waits for
the flash (a page program 0.4 ms, a sector erase 45 ms, each blocking the task one tick at a time)
delay no task with a deadline or an alive indication to give.

**Statistics (`--stats-json`):**
- `handoff`: verdict, present, slot, mode, attempt, image_version, image_version_code, event,
  event_slot, select_write_error, rolled_back.
- `confirm`: state, confirm_after_s, periods_needed, periods, healthy_periods, window_restarts,
  requested_period, the periods that failed each condition (unsupervised_periods,
  not_active_periods, bus_off_periods, unacked_periods), the frames acknowledged in the window so
  far (window_acked) and the fewest in one of its periods (fewest_acked), status, and
  `flash_task` with flash, boot_init, described, have_record, seq, active_slot, trial_slot,
  attempts_used, records_valid, confirms, confirm_status and confirm_ns, and since phase 5
  confirmed_by_routine, self_skipped, idle_passes and, per job of the diagnostic task
  (`jobs`: erase, write, exit, check, confirm), run, last_status, last_steps, last_ns and max_ns,
  and since phase 6 total_ns, their sum.
- `flash`: the operation counts of the host's flash (above).
- `tasks` and `task_table` include `flash`; `telemetry.messages` includes `BMS_BootInfo`.

## The flash tool: fcn_flash

[platform/posix/flash_tool.c](../platform/posix/flash_tool.c), with the same code as the node.
IMAGE is a file written by `python -m fcn_tools.image pack`.

```
fcn_flash factory --flash-file F [--no-wait] [--flash-cut N[:SEED]] IMAGE
fcn_flash stage   --flash-file F [--no-wait] [--flash-cut N[:SEED]] [--address A] [--chunk N] IMAGE
fcn_flash info    --flash-file F
```

- **factory** makes a new flash file; F must not exist.
  - IMAGE is first validated in memory by lib/image. If it does not validate, the tool exits
    with 2 and makes no file.
  - Slot A is programmed page by page, with no erase (the new file is erased already).
  - lib/boot's select then runs once, as the bootloader's first start would, and writes ADOPT:
    slot A holds the CONFIRMED image.
- **stage** makes exactly the calls phase 5's UDS download makes, in the same order:
  1. `boot_slot_by_address()` for `--address`, by default the start of the slot that is not
     CONFIRMED;
  2. `boot_writer_begin()`;
  3. `boot_writer_erase_step()` until it is done;
  4. `boot_writer_write()` in pieces of `--chunk` bytes (default 1024);
  5. `boot_writer_finish_step()` until the image is committed as the TRIAL.

  The next fcn_boot boots it as attempt 1.
- **info** prints JSON (schema `fcn.flash_info/1`): the layout, the metadata as
  `boot_describe()` gives them, and each slot with its recorded state, version and id, lib/image's
  verdict, the header, the validator's id and `id_matches_record`. It only reads.

Every command prints one JSON object on stdout; factory and stage include the flash's operation
counts. Without `--no-wait`, every program and erase waits the modelled time:

| Command | With the waits | `--no-wait` |
|---|---|---|
| factory | 0.336 s (733 programs, 293.2 ms of them modelled) | 4.2 ms |
| stage | 2.413 s (46 sector erases and 735 programs, 2.364 s modelled; 46 erase steps, 183 writes, 2929 finish steps) | 3.3 ms |
| info | 2.8 to 3.7 ms (three runs) | |

Release build, a stripped node image of 187,024 bytes, one run each, the wall-clock time around
the command.

**Exit status:**

| Status | Meaning |
|---|---|
| 0 | done |
| 1 | a usage or start-up error: the flash file is missing for stage or info, exists for factory, or is in use (a node that runs holds it); IMAGE cannot be read |
| 2 | refused by lib/image or lib/boot: the image does not validate, the address names no slot or the confirmed one, a trial is pending. The JSON is `{"command", "status": "refused", "reason", "detail"}` |
| 12 | a power cut fired |

### Reading a flash file without the C code: fcn_tools.flash

[tools/fcn_tools/flash.py](../tools/fcn_tools/flash.py) is a second implementation, written
from this document with nothing but the standard library (and fcn_tools.image for the slots).
It covers the entry sizes, the record, the commit field, which record counts, the used part,
the newest record, the attempts and where the next record goes. `describe(flash_bytes)` returns
what `fcn_flash info` prints, without the tool's operation counts; `python -m fcn_tools.flash
info FILE` prints it as JSON. [tests/python/test_flash_decoder.py](../tests/python/test_flash_decoder.py)
compares the two member by member on a factory flash; 23 failed updates, which take the log round
both metadata sectors (70 records); a cut at every program and erase of an update, with three
seeds; a cut in the bootloader's attempt and in its rollback record; and a slot rewritten behind
the metadata. Every system test of an update reads the flash both ways as well.

## The test payloads

Two programs built by CMake ([tests/tools/payload.c](../tests/tools/payload.c)) stand in for
broken firmware. They are packed into images like the node:

- **fcn_payload_crash** crashes at once: it records PLAT_RESET_IMAGE_FAULT in the cell and exits
  with 13.
- **fcn_payload_watchdog** hangs at start. After 500 ms (the node's hardware watchdog timeout,
  APP_WDG_TIMEOUT_MS) it records PLAT_RESET_WATCHDOG and exits with 11. It is a stand-in: there
  is no watchdog in it.

Like the node at a reset, both release the cell's lock themselves after they have recorded the
reset and before they exit.

Both read the cell as the node does (they take the hand-off) and write one line to `--log-file`,
for example `payload crash: hand-off slot B trial attempt 2, image 1.2.0, event 0; reset cause
IMAGE_FAULT`. They also write a FATAL line to stderr.

## The system tests

`NodeProcess(flash_file=...)` ([tools/fcn_tools/node.py](../tools/fcn_tools/node.py)) starts the
node the way a board with a bootloader starts, through fcn_boot:

```
fcn_boot --flash-file F --log-file <name>.boot.log [--retained-file R] [boot_args] -- <node options>
```

`boot_log()` returns the bootloader's log, and `command()` shows the command line.
`tests/conftest.py`'s `start_node(..., flash_file=, boot_args=)` and
`restart_node(node, name, args, boot_args)` pass these through. `restarted()` keeps the flash
file, so every run after a reset starts through the bootloader again; it does not arm a
command-line `--flash-cut` again, unless `args` or `boot_args` are given. A run that ends with 12
(a loss of supply) counts as a reset only if a cut was armed for it: `--flash-cut` in the node's
or fcn_boot's options, or `NodeProcess.arm_flash_cut(n, seed)` (simulation command flash.cut).
Otherwise `wait_for_reset()` raises and `unexpected_reset()` reports it. The phase 3 checks need
nothing new: after the jump, the process id, exit status, statistics and kicks are the node's.
The bootloader's own statuses are `BOOT_NOTHING_BOOTABLE` (40), `BOOT_FLASH_ERROR` (41) and
`BOOT_JUMP_FAILED` (42). `FCN_BOOT` and `FCN_FLASH_TOOL` override the binaries. The asan run of
`scripts/test-system.sh` uses the asan builds of both; the images are packed from the release
node, because the asan node does not fit a slot.

[tests/system/update_bench.py](../tests/system/update_bench.py) is the bench the update tests
share, since phase 6 a name for [fcn_tools.bench](../tools/fcn_tools/bench.py) and
[fcn_tools.update_model](../tools/fcn_tools/update_model.py), which the measurements of bench/
use outside pytest: the flash tool's commands, `info()` (which reads every flash twice, by
`fcn_flash info` and by fcn_tools.flash, and fails on any difference), `expected_boot()` (what
select must start for the metadata just read, written from the state machine above, and reading
nothing of the code under test), `check_invariants()` (exactly
one CONFIRMED slot, holding its recorded image intact; at most one TRIAL with 0 to 3 attempts
used; every slot whose state names an image holding it intact), the bit flip of the bench, and
the checks of BMS_NodeInfo, BMS_BootInfo and the hand-off against what was expected. The functional
tests decide from what the node itself reports, never from the host's clock, and never from how
far the test process has got while the node runs: a node the test acts on meanwhile runs until
the test stops it, and where the outcome depends on whether the test acted in time (a cut armed at
run time, below, or a bus lost at run time) the test is marked `timing`; a fault that must not
race the node is put in from its start (`--flash-cut`, `--can-fault`), and so is a trial window
that must not race the tester's confirm routine (`--confirm-periods`, above).

| Test file | What it shows |
|---|---|
| [test_boot.py](../tests/system/test_boot.py) | a factory flash starts slot A as the confirmed image (ActiveSlot A, BootMode CONFIRMED, its version) and writes nothing; an erased flash ends fcn_boot with 40 and starts nothing; without a retained cell there is no hand-off (ActiveSlot SNA, BootMode NONE); and the reset cause reaches the image through the bootloader: the node reset by its watchdog and started again through fcn_boot reports WATCHDOG in BMS_NodeInfo, its statistics and RESET_WATCHDOG; an image fcn_boot cannot start (a trial whose payload is no program: 42) leaves no hand-off, so a node started then directly on its flash and cell finds none, writes nothing and confirms nothing, and the next start through fcn_boot gives the trial its second attempt; and a trial, the node or a test payload, that ends at its own start-up checks (a `--log-file` it cannot write: 1) has taken its hand-off all the same; and the node started through `fcn_boot_held` ([tests/tools/boot_held.c](../tests/tools/boot_held.c): fcn_boot whose exec leaves its old memory map, and with it the open file descriptions of both files, alive until the image has ended) locks the cell and the flash and runs, because fcn_boot released its locks before the exec ([bug 010](bug-log.md#010-the-image-found-the-files-still-locked-by-the-bootloader-it-had-replaced)) |
| [test_update.py](../tests/system/test_update.py) | an image staged into slot B starts as a trial, confirms itself after 50 healthy health periods (BMS_BootInfo WAITING with its seconds, then CONFIRMED), and is the confirmed image after a restart; a trial that crashes (13) or hangs until the watchdog's time (11) is started exactly three times and rolled back to A, which reports UPDATE_ROLLBACK in its first BMS_Fault and only in that run; no update can begin while a trial is pending (after a run of the trial that ended by itself before its confirm, lib/boot refuses every address and nothing of the flash changes; while its next run runs, until the test stops it, the tool cannot open the flash the node holds); a trial with one bit flipped in its header or its payload is rejected and never started (REJECT, the DTC); a confirmed image with one bit flipped falls back to the one before it (FALLBACK, the DTC), and the next update goes to the damaged slot; a confirmed image with one bit flipped and nothing before it is not started (40); a trial whose frames nobody acknowledges (started with `--can-fault noack`, simulation control's `can.fault noack` from its start) stays error passive at 128, never bus off, and does not confirm itself in more periods than a window (the node's counts; the flash still holds the trial), and once it is acknowledged again (`can.fault none`) it confirms itself after a whole window that follows every period with the bus lost. Marked `timing`: the confirm is asked for 5 s after the node's start, by its own CLOCK_MONOTONIC log stamps, never earlier than 4.9 s and at most 0.2 s late, in period 50 with no window restart; no period cuts the window short with nothing on the bus but a listener, with the telemetry switched off by a tester, or at 125 kbit/s under a flood above the bus capacity; and the bus lost at run time (`can.fault noack`) once the window has begun cuts it short, and the trial confirms itself after a whole new window once it is acknowledged again (if the command came only after the confirm was asked for, the test process fell behind the node: a missed tolerance) |
| [test_update_cuts.py](../tests/system/test_update_cuts.py) | power cuts on the system, below |
| [test_held_ends.py](../tests/system/test_held_ends.py) | the next start at once after every end of a program that had the flash or the cell open, with the ended program's memory map, and so its mappings of both files, kept alive by a process that shares it (the held variants of [tests/tools](../tests/tools/exit_held.c), [bug 012](bug-log.md#012-the-next-start-found-the-files-still-locked-by-the-run-that-had-just-ended)): `fcn_flash info` after a power cut in `fcn_flash stage`; `fcn_flash info` and fcn_boot after a reset of the node by its watchdog, by a tester's ECUReset and by a power cut in its confirm, after a power cut in fcn_boot, and after the crash payload's end and its start-up error; and after the ends that close the files in order (fcn_boot's 40 and 42, the node's orderly end) |
| [test_flash_file.py](../tests/system/test_flash_file.py) | the flash in the running node: the option, the file, the statistics, the simulation commands |
| [test_download.py](../tests/system/test_download.py), [test_download_faults.py](../tests/system/test_download_faults.py), [test_download_cuts.py](../tests/system/test_download_cuts.py) | phase 5: the update over the bus, end to end and broken on purpose, power cuts during a download ([diagnostics.md](diagnostics.md#how-it-is-tested)) |

### Power cuts on the system

[test_update_cuts.py](../tests/system/test_update_cuts.py) cuts the supply of the flash while the
flash tool stages an update, while the bootloader counts a trial's attempt or writes its
rollback, and while the node writes its confirm (`flash.cut` at run time, and `--flash-cut` on
the node's command line once). After every cut the bench starts the board again through fcn_boot,
as after a loss of supply (status 12 is a power loss: the cut destroyed the retained cell, and the
next start reports POWER_ON), and checks decision 5's invariants from outside: both readings of
the flash agree; the state is the one before the cut operation or the one after it; exactly one
slot is CONFIRMED and intact; the bootloader starts what the state machine says for the state
found, and the node's BMS_NodeInfo, BMS_BootInfo and hand-off say so; a trial is started at most
three times in all and is then rolled back; and the next update can be staged.

| Where | Cuts | What a cut leaves, and what the next start does |
|---|---|---|
| staging (the flash tool) | 13: both programs of the BEGIN record, the first, the last and one drawn erase of slot B, the first, the last and two drawn pages, both programs of the COMMIT record; the commit fields once with a seed that leaves some of the unit and once with one that leaves all of it; the drawn operations and their seeds from a fixed seed | slot A confirmed and started every time; B EMPTY (BEGIN torn), WRITING (anything after BEGIN's commit field), or TRIAL if COMMIT's commit field was cut after its last transition, which is then started as attempt 1 and blocks the next update; otherwise the next update is staged over whatever the cut left. Once, the update staged after a torn erase runs to its confirm |
| the bootloader's attempt | 3, in one sequence with a trial that crashes: an attempt field left erased (not counted, the trial not started), one left partly programmed (counted although the trial never ran), one completed (counted) | the trial runs once in all, its three attempts are used, and the next start rolls it back with the DTC |
| the bootloader's rollback | 4: the record's program, the commit field with none, some and all of it done | the trial is never started again. A torn record: the next start writes the rollback again and reports it. A complete one: the rollback was made, but its report is lost with the start that made it (the hand-off is written after the record), so the next start finds nothing to report, a stated limitation ([faults.md](faults.md)) |
| the node's confirm | 4: the record's program (command line, and at run time), the commit field with some and with all of it done (at run time); the three at run time are marked `timing` (below) | a torn record or commit field leaves the trial with one attempt used, and its second start confirms it; a complete commit field is the confirm, and the next start is of the confirmed image |

That is 24 cuts: 21 in the functional runs of `scripts/test-system.sh`, where the file takes
58 s (release) and 60 s (asan), and the three armed at run time in its timing run, 30 s (the
times of the file's test cases in the JUnit reports of one run of `scripts/test-system.sh -k
"test_update.py or test_update_cuts.py or test_boot.py or test_flash_file.py"`). It is small on
purpose: lib/boot's unit campaign cuts every operation of every sequence at every position,
test_flash_decoder.py every operation of a staged update with three seeds, and phase 6's
[bench/power_loss.py](../bench/power_loss.py) runs the large campaign: 1062 cuts of the real
programs (fcn_flash, fcn_boot and the node serving a download), every flash operation of a
staging, a download and a trial's boot cut several times, each followed by the restart through
fcn_boot, checked against the same reference, and the next update
([docs/measurements.md](measurements.md#power_loss-the-power-loss-campaign)). Its final run
([results/power_loss.json](../results/power_loss.json)) passed all 1062 trials, 400 cuts in a
staging, 200 in a download and 462 in a trial's boot: no invariant violated, every cut where it
was drawn, every next update made, no failure, and no start that found the flash or the cell
still locked (`lock_retries` 0 in every row of `trials`,
[bug 012](bug-log.md#012-the-next-start-found-the-files-still-locked-by-the-run-that-had-just-ended)).
A cut of a one-unit program (a commit field, an attempt)
leaves none, some or all of its transitions done, as the seed says; the seeds of those cuts are
chosen so that each outcome occurs (with lib/flash's model on this geometry, seed 7 leaves none,
0 some and 4 all of a unit's transitions), and the tests check that it does.

A cut armed at run time lands in the confirm only if `flash.cut` is served before the confirm's
first program begins, 5 s after the node's start: only if the test process keeps up with the
node. Those three cases are therefore marked `timing`, and their run is bounded (`--duration-s
8`, longer than the 5 s window), so that a cut armed too late ends the run in order instead of
never.
The test decides from how the run ended. Status 12 with the FATAL line of the operation the cut
was armed for is a cut in time, and the checks above follow. Three ends are a missed timing
tolerance: a run that confirmed and ended in order (the command came after the confirm's programs
had begun); for a cut armed for the record, a cut in the commit field, the run's operation 2 (the
command came after the record's program had begun); and a run that ended in order before its
confirm, the trial still waiting with its window cut short and nothing written (only a host that
held the node up for longer than a health period cuts the window of a working node short, and a
window that begins again after 3 s does not end within the 8 s). Any other end fails the test.
The flash's counts when the command is served could not decide it: a program that has begun but
has not been counted yet is missing from them, and the cut does not count it either.

## How it is tested

| Level | Tests |
|---|---|
| unit (Unity, under ASan and UBSan, and for coverage) | [test_flash.c](../tests/unit/test_flash.c) (the interface and the NOR model, above); [test_sha256.c](../tests/unit/test_sha256.c) (the FIPS 180-4 examples, every length from 0 to 320 bytes against hashlib, every split, the padding boundaries, the 64-bit length); [test_image.c](../tests/unit/test_image.c) (every field at its offset, every rejection in the order of the checks, every flipped bit of an image, steps against the one call, interleaved validations, a failed read at every read); [test_bootimg.c](../tests/unit/test_bootimg.c) (the id, the steps, the results); [test_boot.c](../tests/unit/test_boot.c) (the record bytes, every operation in every state against the reference model, random runs, the ring, the slot writer, the counts above) and [test_boot_cuts.c](../tests/unit/test_boot_cuts.c) (the campaign above), each on two flash models; [test_update.c](../tests/unit/test_update.c) (the confirm's window), [test_fault.c](../tests/unit/test_fault.c) and [test_telemetry.c](../tests/unit/test_telemetry.c) (UPDATE_ROLLBACK, BMS_BootInfo, ActiveSlot) |
| platform | [test_posix_flash.c](../tests/platform/test_posix_flash.c) and [flash_selftest.c](../tests/platform/flash_selftest.c) (the host's flash, above); [test_posix_retained.c](../tests/platform/test_posix_retained.c) (the hand-off: taken once and zeroed, the rules, the bootloader's open that keeps the reason, and the bootloader taking back the hand-off of an image it could not start: none left, the reason and the count kept, nothing changed where there is none; the lock against a forked process: a mapping keeps it after its descriptor is closed, the bootloader's release lets another process take the cell while it is still open and mapped and writes nothing while released, and the lock comes back only while nobody else holds the cell) |
| Python | [test_image.py](../tests/python/test_image.py) (fcn_tools.image against the text and against the C code), [test_sha256_table.py](../tests/python/test_sha256_table.py), [test_flash_tool.py](../tests/python/test_flash_tool.py) (what fcn_flash refuses and leaves), [test_flash_decoder.py](../tests/python/test_flash_decoder.py) (above), [test_simctl.py](../tests/python/test_simctl.py) (the flash commands' client), [test_node.py](../tests/python/test_node.py) (the harness: the bootloader's command line, the restart, the power-loss rule) |
| system | the table above |

Each module was challenged with planted defects in a scratch copy when it was written: 27 in the
flash model, the host's flash and their clients, 16 in lib/sha256, lib/image and fcn_tools.image,
25 in lib/boot and the tests' flash model, 40 in the bootloader, the hand-off, the flash tool,
the payloads, the application's side and the harness, and 7 for the system tests of this phase;
every one was caught by a test. One of the last 7, an attempt limit off by one in lib/boot, at
first survived the system tests: select then tries a fourth attempt, cannot write it, and rolls
the trial back all the same, so that only the hand-off's `select_write_error` shows it; the
system tests check that field since.

## The download over the bus

Phase 5 put the transport on top: ISO-TP (ISO 15765-2) and a UDS server (ISO 14229-1) in the
node's diagnostic task, so that a tester on the bus makes the update that `fcn_flash stage`
makes, with the node running. [diagnostics.md](diagnostics.md) describes the transport, every
service and the tester; this section is what the download does to the flash.

The slot writer's calls are the services' work, each a job of the flash task
([app/flash_job.h](../app/flash_job.h)), one job per service step:

| UDS | Calls (the flash task) | lib/boot's refusals |
|---|---|---|
| RoutineControl 0xFF00 eraseMemory, `31 01 FF 00 44 <address> <size>` | `boot_slot_by_address()`, `boot_writer_begin()` (the BEGIN record), then `boot_writer_erase_step()` until BOOT_OK, one sector per call | no slot starts at the address, or a size of 0 or beyond the slot (BOOT_E_ARG, BOOT_E_SIZE): 0x31 requestOutOfRange; the running slot, a trial pending, the factory state (BOOT_E_STATE): 0x22 conditionsNotCorrect |
| RequestDownload 0x34 | none: it checks that an eraseMemory completed for the same address and size (0x70 otherwise) | |
| TransferData 0x36, blocks of 1024 bytes | `boot_writer_write()` with the block's data at the offset the blocks so far add up to; a repeated block is answered and not written | a refusal ends the download: BOOT_E_SEQUENCE 0x24 requestSequenceError, BOOT_E_FLASH or BOOT_E_POWER_LOST 0x72 generalProgrammingFailure (the whole mapping: [diagnostics.md](diagnostics.md)) |
| RequestTransferExit 0x37 | the first `boot_writer_finish_step()`: the last partial page, the start of the validation | as for TransferData |
| RoutineControl 0xFF01 checkProgrammingDependencies | `boot_writer_finish_step()` until BOOT_OK: the validation (lib/bootimg, one 64-byte step per call) and the COMMIT record: the slot is TRIAL | an image that does not validate (BOOT_E_INVALID): 0x72, the slot stays WRITING |
| ECUReset 0x11 | none: the node resets after its positive response, or at its deadline 2 s after the request if an earlier response still being sent holds that up ([diagnostics.md](diagnostics.md)), and the bootloader boots the committed trial as attempt 1 | |
| RoutineControl 0xF000, the confirm routine (extended session) | `boot_confirm()` of the running trial | no trial to confirm (BOOT_E_STATE): 0x22 |

- **Timing.** The calls run in the flash task, the lowest-priority task, so a sector erase (45 ms
  typical, 400 ms at most) delays no deadline, while the diagnostic task answers responsePending
  40 ms after the request and then every 2000 ms. The tester's timeouts (fcn_tools.diag) are
  computed from the part's maximum times, not the typical ones the host's model waits.
- **Supervision.** The flash task is a supervised entity since phase 5: it gives an alive
  indication after every wait and after every lib/boot call in a job, and a job that never returns
  ends in a watchdog reset, never in a hang ([diagnostics.md](diagnostics.md)).
- **A download is refused while a trial runs or is pending**, for both slots (BOOT_E_STATE): the
  trial's fallback would be overwritten. After the trial's confirm, the slot of the image before
  it is the one a download goes to.
- **An abandoned download is safe.** A session change, S3server, an ECUReset, a refusal, a loss of
  supply: lib/boot leaves the slot WRITING, which the bootloader never starts, and the next
  eraseMemory erases every sector the new image needs again (no second BEGIN record).
- **The DTCs** are served by ReadDTCInformation (0x19: 0x01, 0x02, 0x0A) and cleared by
  ClearDiagnosticInformation (0x14) from the health task's snapshot ([faults.md](faults.md)),
  UPDATE_ROLLBACK among them.

The operations a download makes are those of a stage: for an image of S bytes, the BEGIN record
(its record and its commit field), ceil(S / 4096) sector erases, ceil(S / 256) page programs (the
last, partial one at RequestTransferExit) and the COMMIT record (record and commit field), with no
flash operation of the node's own in between when the running image is the confirmed one.
[test_download_cuts.py](../tests/system/test_download_cuts.py) cuts 11 of them on the system and
checks decision 5's invariants after each, as [power cuts on the system](#power-cuts-on-the-system)
does for the stage; [diagnostics.md](diagnostics.md#how-it-is-tested) has the other tests of the
download.

## What a product would add

This project shows the fail-safe part of an update: that no cut of the supply and no damaged
image leaves the device without an image to start. A product adds at least the following.

- **Signatures.** A valid image proves integrity, not origin: anybody can compute a SHA-256 and
  a CRC. A product signs the digest (or the header that holds it) with a private key that never
  leaves its signing service, and the bootloader checks the signature with the public key it
  carries (or the hash of that key, in one-time-programmable memory) before it trusts the digest;
  MCUboot does this with ECDSA P-256 or Ed25519 over the image's hash, in a TLV after the image.
  The image id would then cover the signature's key, and key revocation needs a plan of its own.
- **Anti-rollback.** The bootloader here starts an older version as readily as a newer one. A
  product that must refuse old, vulnerable firmware keeps a security counter that the firmware
  cannot lower (eFuses, a one-time-programmable counter, or a protected counter in its secure
  storage; MCUboot keeps a security counter in hardware) and refuses images whose own counter,
  inside the signed part of the image, is below it. The counter is raised only once the new
  image is confirmed, or a rollback to the previous image would be refused too. Without a
  signature it means nothing: anybody can write any number into a header.
- **Encrypted images.** The images here are plaintext in transit and in the flash. A product
  that must keep its firmware confidential encrypts the payload (AES in counter mode, as MCUboot
  does, with a key for each image wrapped for the device by ECIES or AES key wrap), and decrypts
  it while it writes the slot, if the slot is in internal flash that executes in place, or while
  it starts it. The signature then covers the plaintext, so that the bootloader checks what will
  run.
- **Updating the bootloader itself.** The bootloader is not updated by this design: a cut while
  it was rewritten would leave nothing that can start anything. A product either never changes it
  (a small immutable first stage in ROM or write-protected flash, the root of trust, that does
  little more than check and start the second stage), or keeps two copies of the second stage and
  lets the first stage choose between them with the same kind of records and attempts as here.
  Either way the first stage's sectors are write-protected (on an STM32 the option bytes' write
  protection), so that no image, broken or hostile, can overwrite it.
- **More than this project checks.** The hardware revision an image is built for (a field of the
  header), a confirm made by a tester's routine or after a drive cycle rather than after 5 s, and
  the endurance of the part actually used.

## Limits

- **The flash is a model.** No weak bits, retention errors, read disturb, suspend, wear-out or
  ECC (the NOR model's list above), and typical times only.
- **The bootloader is a host program.** It starts its image with `fexecve()` from a sealed copy;
  the jump on a target, and a bootloader that cannot be updated, are phase 7's.
- **No access control on the download.** Any tester on the bus can erase the other slot and write
  an image into it: there is no SecurityAccess (0x27) in front of the programming session
  ([diagnostics.md](diagnostics.md#not-implemented-and-what-a-product-adds)).
- **Integrity, not authenticity, and no anti-rollback** (above).
- **The rollback's report can be lost.** UPDATE_ROLLBACK is reported by the start right after the
  rollback only, from the hand-off. If that start never reaches its first BMS_Fault (a cut of the
  supply after the bootloader's rollback record and before the image runs, or a reset before the
  first health period), the next start has nothing to report; the metadata still show it (the
  newest record's event, `fcn_flash info`). DTCs are not kept in flash in this phase
  ([faults.md](faults.md)).
- **The self-confirm proves that the trial is heard, not that it hears.** Its frames must be
  acknowledged in every period of the window; what it receives plays no part (the confirm,
  above). The tester's confirm routine proves the receive path, but only if a tester confirms
  first: a trial that receives nothing still confirms itself after 5 s.
