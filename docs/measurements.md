# Measurements

Every figure the README quotes comes from a file in [results/](../results), written by a bench
in [bench/](../bench). This document says, for every bench, what it measures, exactly how
(where each time is taken, by which clock, in which process, what an interval includes and what
it leaves out), under which conditions, how many trials and why that is enough, what the figure
does and does not say about a microcontroller, and the command that makes it. Each bench's
section also gives what its final run measured (**Results.**), read from results/*.json, the
final runs on a committed tree ([the final runs](#the-final-runs)); the README quotes its
figures from the same files.

Every figure is a measurement of the FreeRTOS POSIX port, where every task is a thread of a
Linux process on a WSL2 virtual machine of the development laptop. Every result carries this
sentence (`HOST_TIMING_NOTE`, [tools/fcn_tools/results.py](../tools/fcn_tools/results.py)):

> Measured on the FreeRTOS POSIX port, where every task is a thread of a Linux process. Times,
> jitter and latencies are those of the host's scheduler and are not what the same code shows on
> a microcontroller. The contents, orderings and sizes of protocol data, and the frames and bytes
> an exchange takes, do not depend on the host. What happens in a stretch of time does: the jobs
> and frames of a run of fixed length, ticks dropped, jobs released early; and a stall of the
> host past the node's 500 ms watchdog or a 1 s protocol timer fails what the node would have
> passed. Such a failure is never hidden (the bench counts and keeps it, or writes no result),
> and no stall makes a check pass, so a count of 0 failures holds whatever the host did.

and a sentence of the bench's own where its figures say more or less than that.

| bench | what it measures | conditions | default run |
|---|---|---|---|
| [node_timing](#node_timing-latency-and-jitter-cpu-load-stack) | release jitter of the periodic tasks, latency from a sample to the bus and to an observer, periods of the cyclic frames, CPU load per task, stack high-water marks | idle bus, a flood at half the bus, during a download; SocketCAN and UDP | 120 s idle, 120 s flood, 3 downloads, 60 s without the trace, per transport |
| [bus_load](#bus_load-the-load-on-the-bus) | every frame on the bus with its exact length: load per identifier and in all, against the DBC's nominal load | idle bus, during a download; SocketCAN and UDP | 60 s idle and 1 download per transport |
| [bus_off](#bus_off-the-way-into-bus-off-and-the-recovery) | the way into bus off and the recovery, against ISO 11898-1's 1408 bit times; the hold-off and its doubling; what the host adds | a short and 32 destroyed transmissions at 500, 125 and 1000 kbit/s, UDP | 100 trials of each fault per bit rate, and a short held through 7 bus offs per bit rate |
| [isotp_throughput](#isotp_throughput-the-transfer-rate-of-iso-tp-and-uds) | TransferData rate by block size, each block cut into the turns of both sides; the node's longest response; a labelled projection for 2048-byte blocks | idle bus but for the node's telemetry; SocketCAN and UDP | 10 block sizes of 64 blocks, 500 responses, per transport |
| [updates](#updates-end-to-end-and-the-256-kib-image) | updates over the bus end to end, step by step; the download of an image whose payload is 256 KiB | SocketCAN and UDP; the 256 KiB image over SocketCAN | 50 updates over SocketCAN, 10 over UDP, 10 downloads of 256 KiB |
| [power_loss](#power_loss-the-power-loss-campaign) | whether a cut of the supply in any flash operation of an update leaves a state the design does not allow, or a board that cannot boot and be updated again | the real programs, in 8 parallel workers | 1062 trials: every operation of each class cut 8, 4 or 33 times |
| [platform_timing](#platform_timing-the-host-mechanisms) | tick accuracy (the stock port and the locked tick), simulated interrupt latency, an interrupt storm | the platform self-test | 4 runs of 10 s and 2 storms of 5 s |

## How every bench works

Every bench is a script `bench/<name>.py`, run from the repository root with the project's
Python ([bench/README.md](../bench/README.md) has the commands). They share one skeleton
([tools/fcn_tools/results.py](../tools/fcn_tools/results.py),
[tools/fcn_tools/runner.py](../tools/fcn_tools/runner.py)), so that every result can be trusted
and repeated in the same way.

**The binaries it measured.** Each host program carries the build id of the tree it was built
from ([platform/posix/build_id.h](../platform/posix/build_id.h), written by
[cmake/FcnBuildId.cmake](../cmake/FcnBuildId.cmake) at every build). The id is the commit; a
tree with uncommitted changes to tracked files (other than results/), the submodules' included,
adds `-dirty-` and the first 12 hex digits of the git hash of those changes. The node reports
it in `--version`, in a log line at its start and in its statistics (`build_id`); fcn_boot in
`--version` and its log; fcn_flash in `--version`; plat_selftest in every report.

A bench builds the release preset first, unless it is told which binaries to use (`--no-build`,
or `FCN_NODE`, `FCN_BOOT` and `FCN_FLASH_TOOL`). It then refuses to measure (exit status 4) when
one of them does not carry the id of the tree that `fcn_tools.results.tree_build_id()` computes
by the same rule, and when a node that ran (also one started through fcn_boot from a packed
image) reports another one in its statistics. `write_result()` checks again just before and just
after it writes the result, against the provenance it writes, and refuses a binary whose SHA-256
changed while the bench ran. A result therefore names exactly the commit, or the dirty tree, its
binaries were built from. The id covers the tracked files and the submodules: the FreeRTOS
kernel, whose POSIX port the node runs on, and Unity are submodules (third_party/), and one
checked out at another commit than the tree records, or with a tracked file changed in it, makes
the tree dirty, its own diff part of the hash. No untracked file counts, in the tree or in a
submodule, as the build compiles only files that tracked CMakeLists.txt name. The final runs are
made on a committed tree ([bench/run_all.py](../bench/run_all.py) refuses a dirty one), so every
final result names a commit. The verification of phase 6 planted each case and saw the refusal,
with nothing written: binaries built from another tree (exit 4 within a second, before
measuring, for every bench; platform_timing exit 1), a tracked file edited after the build (exit
4), and a tracked file edited while a bench ran (exit 4 when it came to write).

**The script that computed it.** The commit a result names is that of the checkout `fcn_tools`
is imported from, and the bench script computes the figures (isotp_throughput.py and updates.py
keep their analysis functions in it). So a bench also refuses to measure (exit status 4)
unless its own script is the file at the same path in that checkout, byte for byte
(`fcn_tools.results.script_mismatch()`, in `require_tree_build()` and `write_result()`). A
script of another checkout (to which the virtual environment's editable install gives this
checkout's `fcn_tools`) or of no checkout is refused, so the script the result's `command`
names, in the checkout whose commit the result names, is the one that computed it.
platform_timing refuses it when it comes to write (exit 1), and bench/run_all.py before it
starts anything. The bytes are compared, not the paths: under WSL the same tree is reached
through two mounts (/home/dev/fcn and /mnt/c/...).

**The host.** Timing benches assume an otherwise idle machine. The 1-minute load average is
read when the bench starts, before its build and before it measures, and is recorded in
`provenance.host` with the one at the end. Above a limit (`--max-load`, default 1.0) the bench
refuses (exit status 3), unless `--allow-loaded` makes it measure and mark the result
`"loaded": true` with the limit; a loaded result is not quoted. This VM (8 logical CPUs) reads
0.0 to 0.4 when idle; a build or a test run of the project shows 2 to 8, and one busy loop adds
1. The load average sees only the processes of the WSL VM, not what Windows runs beside it.

**The result.** `write_result()` writes `results/<name>.json` whole or not at all (a temporary
file renamed over it), with these members in this order: `schema` (`fcn.result/<name>/<version>`);
`provenance` (command, commit, modified files, host, load averages); `host_timing_note`;
`loaded` and `load_limit`; `build` (the tree's id, each binary's path, id and SHA-256, the ids
its runs reported); `config` (every parameter that shapes the numbers: bit rate, transport,
durations, trial counts, seeds, image sizes); `summary` (the figures the README may quote, each
with its unit, which `write_result()` enforces); and the bench's data. Each bench also writes
`<name>.png` with [fcn_tools.plotstyle](../tools/fcn_tools/plotstyle.py), and `--replot`
redraws it from the JSON. A `--quick` run tests the bench in seconds and must be given
`--out DIR`, so that results/ only ever holds full runs.

**Distributions.** A distribution is given as `n`, `min`, `p50`, `p90`, `p99`, `p99_9`, `max`,
`mean` and `stdev` (the sample standard deviation, n − 1), with its unit
(`fcn_tools.results.distribution()`; percentiles by numpy's linear interpolation).

- A percentile p is given only when at least five samples lie beyond it, n·(1 − p) ≥ 5: p50
  needs 10 samples, p90 50, p99 500 and p99.9 5000; below that it is null. With fewer samples
  beyond it, the "percentile" would be one of the largest few values seen, as uncertain as the
  maximum itself.
- The standard deviation needs two samples.
- The samples themselves are kept in the JSON up to 5000 of them; above that a histogram of 200
  equal bins from the smallest to the largest sample, whose counts add up to n (`raw_samples()`).
- A figure pooled over runs of different lengths is pooled, not averaged: frames over the total
  window, running time over the total time.
- Every figure is computed by a function that tests/python tests on constructed data with a
  known answer.

**Running the node.** A bench starts the node as the system tests do (`fcn_tools.runner.Session`):
in a working directory of its own under the build root; on UDP ports of its own
([fcn_tools.ports](../tools/fcn_tools/ports.py)), or on vcan0 under the same lock file the tests
take, so that a bench and a test run never share the interface; directly, or through fcn_boot as
a Board that follows requested resets. It reads the node's statistics after an orderly stop. A
session that ends normally fails when one of its nodes ended without the bench seeing it, did not
stop in order, was reset unexpectedly, or lost a watchdog kick: the same end checks as the tests.
A measurement of a node that went wrong unseen is never written. On every way out (an exception,
Ctrl-C, SIGTERM or SIGHUP to the bench) every node is stopped in order or killed, and the ports
and the interface are released; the node asks the kernel to kill it when the process that
started it dies (PR_SET_PDEATHSIG), which covers a bench killed with SIGKILL.

**The references.** What the bootloader must decide for a flash, and what every flash must
satisfy (`fcn_tools.update_model.expected_boot()` and `check_invariants()`), is written from the
state machine of [docs/update.md](update.md). It reads only the description of the flash that
`fcn_flash info` and the independent decoder fcn_tools.flash agree on, never the code under
test. It moved from the update tests to fcn_tools in phase 6 unchanged, so that the benches and
the tests judge by the same rules. The diagnostic tester (`fcn_tools.tester`) is can-isotp 2.0.7
carrying raw requests, and the flasher is udsoncan 1.26.1 over it.

**Exit statuses.** 0 a result was written; 1 an error (the bench says what; nothing written); 2 a
usage error; 3 a loaded host; 4 a binary that is not the tree's build, or a bench script that
is not the tree's; 5 (power_loss and updates only) a result was written that holds a failed
trial or update, which must be looked at; 128 + N the bench was ended by signal N.
bench/run_all.py stops at the first status other than 0.

## The clocks of a run

Three kinds of time appear in the results, and every figure says which it uses.

- **The node's CLOCK_MONOTONIC.** Every time the node or its host layer records (its log, its
  statistics, the host-only traces) is `clock_gettime(CLOCK_MONOTONIC)` of the VM, read in the
  node's process where the event happens. A bench's own times (`time.monotonic()`) are the same
  clock, read in the bench's process, so the two can be subtracted.
- **The kernel's receive time of a frame.** An observer of the bus asks the socket for it
  (SO_TIMESTAMPNS): the moment the frame was queued to the observer's socket, not the moment
  Python read it. On vcan0 that is when the sender's write reached the interface's loopback; on
  the UDP bus when the datagram reached the observer's port. It excludes the observer's own
  wake-up. The kernel gives it on CLOCK_REALTIME, the wall clock.
- **The wall clock steps.** Under WSL2 the VM's wall clock is set from the host's again and
  again; the engineers' pilots saw steps of about a second every half minute or so (once 1.79 s,
  and once over two hours), and the verification's rehearsal about one per node run.
  CLOCK_MONOTONIC never steps. Every bench that uses kernel receive times
  handles the steps, each in its own way below, and counts what it could not use.

`python -m fcn_tools.capture` ([tools/fcn_tools/capture.py](../tools/fcn_tools/capture.py)), the
observer of node_timing and bus_load, is a process of its own (so that it does not miss frames
while the bench's flood thread or the flasher run). After every frame it reads the offset of
CLOCK_REALTIME from CLOCK_MONOTONIC: a MONOTONIC, a REALTIME and a MONOTONIC reading, the
REALTIME one against the middle of the other two, and the time between those two as the
reading's bracket. A frame's time on CLOCK_MONOTONIC is its kernel time less the offset
(`to_monotonic()`):

- A reading with a bracket of at most 2 µs is good. The offset changes only when the wall clock
  steps, so a good reading whose offset differs by more than 2 µs from the last good one marks a
  step.
- A frame read with a good reading takes its own offset. A frame whose reading was interrupted (a
  wider bracket, whose offset can be off by half of it) takes the offset of the next good reading,
  provided the good readings on both sides agree, so that no step lies between them.
- Not trusted, and counted (`frames_time_not_trusted`, `intervals_time_not_trusted`): the frame
  at which a step was seen (it may have arrived before the step); a frame with an interrupted
  reading whose next good reading saw a step, or that has no good reading on one side; a frame
  whose time would come after its own read; and a frame whose time lies before the read at which
  the last step was seen, which covers a backlog of frames read one after the other across a
  step, each with the new offset.
- A capture in which the kernel dropped frames (SO_MEMINFO) or a datagram that is no frame of
  the bus is refused.

Untrusted frames still count for matching and for the counts; they are left out of the timing
figures only. The verification of phase 6 found that the first version of this rule, which left
out every frame with an interrupted reading, dropped most BMS_PackStatus frames (the first frame
after the observer had waited, whose reading is the one most often interrupted), and with them
the p99.9 that the counts below promise; the rule above uses them.

The VM's clock was checked against Windows' just before the final runs (2026-11-23, 18:26 to
18:40, WSL freshly restarted, on AC power, no standby), outside the benches, with two PowerShell
scripts on the Windows side:

- CLOCK_MONOTONIC of the guest (/proc/uptime) against Windows' Stopwatch over 180 s, each
  reading taken at the midpoint of its wsl call: 174.300 s of guest time against 180.117 s of
  Windows time, −3.230 % (±0.059 %, from the durations of the calls), with the guest's
  clocksource tsc; with hyperv_clocksource_tsc_page, switched to for this test only, −4.007 %
  (±0.074 %).
- A guest ticker printing a line every 0.2 s of guest time for 90 s, each line stamped by
  Windows on arrival: 449 steps, the median guest step 0.2000 s and the median Windows step
  0.2007 s (a rate of −0.353 %), no Windows step over 0.3 s (the longest 0.221 s), and in all
  89.800 s of guest time against 93.456 s of Windows time, −3.912 %.

The guest clock runs at the right rate to within about 0.35 %, and the VM loses about 3 to 4 % of
wall time in short freezes, of up to about 20 ms each, in which guest time does not advance.
Everything the benches measure runs inside the VM on its CLOCK_MONOTONIC, so the node, the
benches and vcan0 alike see a freeze as time that did not pass. It is a property of the host,
covered by calling every time here host timing; no result corrects for it.

## node_timing: latency and jitter, CPU load, stack

**What it measures.** The timing of the node's own work on the host:

- the release jitter of its periodic tasks, sensor (10 ms) and health (100 ms), and of the
  processing task, which the sensor task's samples release;
- the latency of its data path, from a sample to the frame that carries it on the bus and at an
  observer;
- the period of every cyclic frame on the bus;
- its CPU load per task;
- the stack high-water mark of every task.

It measures each on an idle bus, under a flood of other traffic and during a download, on
SocketCAN (vcan0) and on the UDP bus.

**How: the node's trace.** The release node runs with `--trace-file`
([platform/posix/posix_trace.h](../platform/posix/posix_trace.h)), a host-only trace defined
through the kernel's own trace macros in
[config/posix/FreeRTOSConfig.h](../config/posix/FreeRTOSConfig.h); the kernel and app/ are
unchanged. Every event's time is read with `clock_gettime(CLOCK_MONOTONIC)` where the event
happens. It records:

- every task switch, in `vTaskSwitchContext()`, with whether the task switched out had blocked
  (it was in no ready list) or had been preempted;
- every move of a task to the ready list, in `prvAddTaskToReadyList()`, with the tick count;
- every block in `xTaskDelayUntil()`, with the tick to wake at;
- every frame of the node that the controller model completed, in the CAN hardware thread, with
  the model's end of frame and the time it was written to the transport;
- every sensor read, in `plat_sensor_read_current()`, with the sample's number;
- when every tick was due: posix_tick.c's base (tick k is due at base + (k − 1) ms) and every
  shift of it after a dropped tick.

Records go into a buffer written once before the scheduler starts (32 bytes each, at least
2^20 of them and 30,000 per second of the run) and are written to the file after the run, so the
run does no I/O for the trace; a run that drops a record is refused. The node times the cost of
one record when it opens the trace and reports it with the statistics (`trace.event_cost_ns`):
the fastest of 8 rounds of 4096 records, in a tight loop with the buffer warm, so a best case
that leaves out the moments the host took the thread away and the cache misses of a record made
in the middle of a task's work. Each run's result gives its events per second and the share of
time they would cost at that rate, a lower bound; the control run without the trace is the check
that holds under the run's own conditions, as it lets the periods on the bus and the CPU shares be
compared with and without the trace. Without `--trace-file`, each hook costs one relaxed atomic
load.

**How: the observer.** [The capture](#the-clocks-of-a-run), a raw CAN socket on vcan0 or a
participant of the UDP bus, keeps every frame with its kernel receive time on CLOCK_MONOTONIC.

**The intervals, exactly.**

- *Release jitter* (sensor, health). For each job:
  - its arrival is the due time of the tick its `xTaskDelayUntil()` waited for, on the base in
    force for that tick (see *Ticks given up* below);
  - its release is the moment the kernel made it ready (the tick handler, in the thread the
    tick's signal interrupted);
  - its start is its first switch-in after that;
  - its end is the switch-out at which it blocked;
  - its execution is the time it ran between start and end, the same running intervals
    FreeRTOS's run-time counter counts.

  Release lateness is release − arrival, start lateness is start − arrival, start after release
  is start − release (what the scheduler added once the task was ready, whatever the tick's own
  error against the clock), response is end − arrival, and the period deviation is the interval
  between two consecutive starts minus the period. A job that blocks on the pack mutex in the
  middle is one job.

  A switch is recorded where the kernel decides it, in `vTaskSwitchContext()`. The port then
  hands the processor over by resuming the task's host thread (it signals the thread's condition
  variable and suspends the one that ran), and the thread runs once the host schedules it. That
  hand-over is in neither the start lateness nor the start after release: they end at the
  kernel's decision, which for the sensor task (priority 6, below only can_rx) comes in the same
  tick handler as its release unless can_rx is ready or the scheduler is suspended. It is in the
  job's execution and response instead, as it is in FreeRTOS's own run-time counter, which counts
  between the same switches.
- *Releases before they were due.* posix_tick.c makes up a tick it takes for lost. When that
  tick was only late (pending on the thread of a task that was switched out), the kernel holds
  one tick more than the clock from the moment the late one is taken, and a task released at
  that count is released before its tick was due. The tick thread gives the tick back at the
  next tick boundary: there, before it raises the next tick, it reads the count again and skips
  the raise if the kernel already holds that tick
  ([posix_tick_plan.h](../platform/posix/posix_tick_plan.h)). A late tick can therefore make a
  release early by less than one period, and only in the period in which it was taken; two late
  ticks taken in the same period put the count two ahead, one is given back at each of the two
  boundaries after, and a task released at the second extra tick is released one to two periods
  early. (Until
  [bug 011](bug-log.md#011-a-tick-made-up-for-a-late-one-kept-the-count-a-tick-ahead-of-the-clock)
  was fixed the give-back rarely worked and the count stayed a tick ahead for long stretches.)
  The bench measures this as it is: such a job's release and start lateness are negative and
  stay in the distributions, and the result counts these jobs (`released_before_due`) and, of
  them, those released more than one period and 100 µs early (`EARLY_MARGIN_NS`, room for the
  clock reads), which takes the count two or more ahead (`released_more_than_a_tick_early`); the
  summary gives both for the sensor task (`sensor_released_before_due`,
  `sensor_released_more_than_a_tick_early`, of `sensor_jobs`). Per run it gives the ticks the
  node made up and gave back (`tick_catch_ups`, `tick_give_backs`, next to `tick_dropped`). Start
  after release is not shifted by it, and is the figure for the kernel's own delay (to its
  decision, see above); release and start lateness are the figures for the timing against the
  clock, the tick's error included. The period deviation, between consecutive starts, is not
  shifted by it, but for the two intervals on either side of a job released early.
- *Ticks given up* (`release.<task>.after_drop`). When the kernel takes no tick for several
  periods (the host did not run the task threads, or a task kept a critical section), the tick
  thread makes the missing ticks up one at a time, 100 µs apart, as long as the count trails the
  ticks due by at most `POSIX_TICK_MAX_CATCH_UP_TICKS` (6,
  [posix_tick_plan.h](../platform/posix/posix_tick_plan.h)). A longer lag it gives up, all of it
  but the tick just raised (at least 6 ticks, counted in `runs[].tick_dropped`): it moves the
  base later by as many periods, and from the tick after the ones the kernel had taken every
  tick is due on the moved base. Each job is measured on the base in force for its own tick. On
  the grid before the drop every later job would carry the whole shift, which means nothing for
  any of them but the first. On the shifted grid, though, a job's lateness keeps at most the part
  of such a stall that was caught up, a lag of at most `POSIX_TICK_MAX_CATCH_UP_TICKS` ticks; the
  ticks given up show in the period deviation (between the starts of the jobs on either side of
  the stall) and in `after_drop`. That list holds, per periodic task and over the runs of a
  condition, every job whose tick is due on a later base than the tick of the job before it (for
  the task's first job, than the trace's first base): the first job after one drop or more. For
  each it gives the run (`run`), the base's first tick
  (`first_tick`), the drops since the job before (`drops`, 1 unless a second drop came before
  the task's next job) and the ticks they gave up (`dropped_ticks`), the tick the job waited for
  (`wake_tick`), whether it is in the analysis window and so in the distributions (`in_window`:
  the list covers the whole run, as `tick_dropped` does), and its release and start lateness on
  its own base, as the distributions hold them (`release_lateness_us`, `start_lateness_us`), and
  on the base of the job before (`release_lateness_on_the_base_before_us`,
  `start_lateness_on_the_base_before_us`), larger by exactly `dropped_ticks` periods. The
  summary counts the jobs of the list in the window (`sensor_first_jobs_after_a_drop`,
  `health_first_jobs_after_a_drop`). The processing job that such a sensor job's sample released
  is measured from the same arrival, on the moved base. A drop after a task's last job in the
  trace has no entry, so the `dropped_ticks` of a run's entries add up to its `tick_dropped` or
  less.
- *The tick count against the grid* (`tick_lead`, per condition, over the whole of each run, its
  start included). Every SWITCH and READY event of the trace carries the kernel's tick count,
  and each count is taken at its first sight
  ([fcn_tools.trace.tick_lead()](../tools/fcn_tools/trace.py)): the counts the trace saw
  (`counts_seen`), those first seen before their tick was due (`seen_before_due`: the count was
  ahead of the clock), those seen more than a period and 100 µs before (two or more ahead,
  `seen_more_than_a_tick_before_due`), the most a count was seen early (`most_before_due_us`),
  and the longest stretch with the count ahead (`longest_stretch_ahead`, in ms): consecutive
  counts each first seen before their tick was due, from the first sight of the first to the
  time the last one's tick was due. The trace sees the count only at these events, so a stretch
  begins at a sight, which can be after the count got ahead, and a count it never saw (followed
  by the next before any event) is taken to be like the counts around it. Nor is a stretch a
  time with the count ahead throughout: it goes on as long as each new count is first seen
  before its tick was due, also where the count stood on the grid for a moment in between (with
  the old give-back of bug 011, at every boundary until the tick raised there was taken).

  With the fix of bug 011 a correct node cannot keep the count ahead for long. The tick thread
  raises no tick at a boundary while the kernel already holds it, and makes none up unless the
  count is behind, so a count ahead rises only by ticks that were already pending, at most one
  on each task's thread (a second SIGALRM to a thread that holds one merges with it): with the
  node's 8 tasks it is back on the grid within 8 periods. In the verification of that fix
  (`--seconds 24 --downloads 1 --control-seconds 12` rehearsals on the laptop on battery, the
  bench's traces read with this rule), the longest stretch per condition was 0.7 to 1.9 ms with
  the fix (27 conditions of 5 runs) and 250 ms to 1.8 s with the old give-back (24 conditions of
  4 runs); counts seen more than a period early came 9 times in 7 of the 27 conditions with the
  fix. The bench refuses a run with a stretch longer than 50 periods (`LEAD_LIMIT_PERIODS`, in
  `config` as `lead_limit_periods`), which a node that keeps its made-up ticks gives (each of the
  24 conditions of the old give-back above). Before this rule the bench refused every release
  more than a tick and 100 µs early instead: in those rehearsals 1 of the 4 runs of the old
  give-back and 2 of the 5 runs of the fix, each for one sensor job 1.5 to 1.7 ms early with the
  count two ahead. The two conditions of the fix that were refused so (both flood runs) have
  longest stretches of 1.6 and 1.9 ms; their jobs are now counted.

  The rule also refuses a trace whose grid falls due most of a period or more later than the
  kernel's ticks (a TICK_BASE event with too late a time), but not every trace whose grid is
  wrong. A base less late keeps ahead only the counts seen soon enough after their tick: moved
  later in those two flood traces and in the download run of a `--seconds 24 --downloads 1
  --control-seconds 12` rehearsal of this rule (on AC power), by 0.8 ms or more it was refused
  in all three, by 0.2 or 0.5 ms only in the download run. Nor does the rule refuse a trace that
  missed a base: a drop gives up at least 6 ticks and moves the base later, so the counts after
  a missed base look late, not ahead. Left out one at a time, the two later bases of that
  download run (34 ticks dropped) made the sensor's releases after them about 15 and 19 ms late,
  which the bench reports as lateness and does not question.
- *The processing task.* Its jobs are those the sensor task released (its sample's queue send),
  measured from that sensor job's arrival, which is how the schedulability check counts its
  response. A stretch released by the health task (the pack mutex given back) continues the job
  before it; one released by its 30 ms receive timeout is counted (`not_released_by_a_sample`)
  and left out.
- *Latency.* For each frame of BMS_PackStatus, BMS_CellVoltages1/2 and BMS_Temperatures:
  - sample → transmit done: from the release of the sensor job that took the frame's sample to
    the model's end of frame, when a real receiver would have the frame;
  - sample → observer: from the same release to the observer's kernel receive time, which
    includes the transport's delivery and excludes the observer's wake-up;
  - both also from the job's arrival (adding the tick's own lateness), and the delivery from end
    of frame to the observer on its own.

  The frame's sample is found by its E2E Profile 11 counter. The k-th publish of a message
  carries counter k mod 15 and the k-th sample (every second sample for the cells, every tenth
  for the temperatures). The counters are unwrapped along the bus, the gaps are held against the
  node's count of superseded frames, and each sample is identified by its read in the trace; a
  second sample read in the same sensor job (an overrun pass) has no release and is counted and
  left out. The bench refuses a run with a sample lost at the full queue, a suppressed or failed
  publish, more frames of a message in the trace than the queue sent (beyond the three a stop can
  leave), more gaps than supersessions, a frame that ends before its sample was read, a latency
  above 1 s, a median latency of a sensor period (10 ms) or more, or an observer that did not see
  exactly the node's frames in the node's order and with their content. A frame ending before its
  sample shows a match that is late by a sample; a match early by a sample breaks no such rule
  and makes every latency a period too long, which the median shows (a node whose frames reach
  the bus a period late more often than not would have missed every deadline of its table, and
  a single frame held up by the host does not move the median). The verification of phase 6
  planted both in a scratch copy (every frame matched to the sample after, and to the sample
  before, its own) and saw both refused; before the median check, the early one had passed with
  every latency 10 ms too long.
- *Periods.* For every cyclic identifier of the node: the intervals between consecutive arrivals
  at the observer, minus the DBC's cycle time; an interval with an untrusted end is left out and
  counted.
- *CPU.* FreeRTOS's run-time statistics (`run_time_us` per task over `run_time_total_us`), pooled
  over the runs of a condition, the idle task's share among them; the run time per job (run time
  over the jobs the trace counted over the whole runs), next to the task table's design budget,
  which is per job, or for can_rx, can_tx and diag per window of the table's period. From the
  trace, within the analysis window: each task's share (pooled over the runs' windows), each
  job's execution time, and each task's demand in consecutive windows of its table period on
  CLOCK_MONOTONIC.
- *Stack.* `stack_peak_bytes − stack_start_bytes` (what the task's code used of its host thread's
  stack) against `stack_size_bytes` (the stack it was created with, meant for the target), and
  the margin; the maximum over every run of every condition.

**The conditions.**

- *Idle:* the node alone with the observer, `--seconds` (120 s).
- *Flood:* the same with frames of 0x700 (below every frame of the node in priority, 8 bytes of
  0x55) offered at half of the 500 kbit/s bus by `fcn_tools.bench.Flood`, 120 s.
- *Download:* `--downloads` (3) runs, each started through fcn_boot from a flash holding the
  factory image. In each, the flasher (`--no-reset`) downloads the release node, stripped (an
  image of 252,560 bytes in the final run), into the other slot; the figures cover the download
  from the tester's first request frame to the node's last response frame at the observer.
  `--no-reset`, because a run that ends in a reset writes no statistics.
- *Control:* the idle run again without the trace, `--control-seconds` (60 s).

Idle, flood and control figures are taken from 1 s after the node's first heartbeat at the
observer to its last heartbeat, and the download's from the tester's first request frame to the
node's last response frame; only frames whose times are trusted mark these edges. (The
verification of phase 6 found the edges taken from every frame: in one of its quick runs a
heartbeat read across a step of 1.49 s had a time 0.92 s before the first heartbeat's read,
which put the start of the window into the node's start-up, 392 sensor jobs where 3 s hold
300.) Every run is at 500 kbit/s with sensor seed 1.

**Counts, and why they are enough.**

- 120 s give about 11,900 sensor jobs and BMS_PackStatus frames per condition and transport,
  enough for p99.9 (5000 needed); the cell messages, about 5,950 frames, have it too.
- Health jobs and temperature frames, about 1,190, have p99 and no p99.9.
- The three downloads give about 7,000 sensor jobs (6,988 per transport in the final run) and
  about 700 health jobs (699 and 698; p99 only).

**What it says about a microcontroller.** The orderings carry over: which frame carries which
sample. The times do not, and neither do the counts of a run: its jobs and frames, the
publishes missing on the bus, the ticks dropped and the jobs released early are what the host
let the node do in the run's duration (a host that makes the node give up ticks leaves its
10 ms jobs short of 100 a second).

- Every time here is that of a Linux thread under the host's scheduler. A job "runs" in
  FreeRTOS's view while the host may have taken its thread away, and the tick and the simulated
  interrupts are signals.
- The execution times and CPU shares are x86-64 times and say nothing about a Cortex-M3; the
  task table's budgets stay design figures (see [the limits](#what-the-figures-do-not-say)).
- The stack depths are those of host threads with x86-64 frames. They include the signal frames
  of the port's tick and interrupt signals, which land on whichever task runs and hold the
  thread's whole register and floating-point state. A Cortex-M3 takes far less from a task's
  stack: on exception entry the hardware stacks an 8-word frame (32 bytes, no FPU, and one word
  of padding when it realigns the stack to 8 bytes) onto the stack in use, under FreeRTOS the
  task's process stack (PSP); the handler then runs on the main stack (MSP), and so does any
  exception nested in it; a context switch saves r4 to r11, 8 words more, on the task's stack. A
  host peak above a task's configured size is therefore no overflow of the target; the target's
  own high-water marks are for phase 7's Cortex-M3 build.

**Results.** The final run is [results/node_timing.json](../results/node_timing.json), drawn in
[results/node_timing.png](../results/node_timing.png). Every time below is host timing of the
POSIX port (the result's `host_timing_note`); the counts are not. Each idle and flood run had
11,899 sensor jobs (11,898 in the UDP idle run) and 1,189 health jobs. The summary's
distributions (`summary.<transport>.<condition>`), in µs, p50 / p99.9 / max unless the row says
otherwise:

| µs | SocketCAN idle | SocketCAN flood | SocketCAN download | UDP idle | UDP flood | UDP download |
|---|---|---|---|---|---|---|
| sensor release lateness | 42 / 334 / 570 | 33 / 848 / 1204 | 32 / 398 / 2164 | 45 / 278 / 1855 | 33 / 900 / 3485 | 34 / 371 / 1315 |
| sensor start lateness | 48 / 363 / 570 | 36 / 850 / 1204 | 37 / 438 / 2164 | 50 / 312 / 1856 | 36 / 903 / 3521 | 38 / 438 / 1316 |
| sensor start after release | 0.6 / 117 / 230 | 0.2 / 131 / 350 | 0.4 / 105 / 150 | 0.6 / 141 / 317 | 0.3 / 120 / 178 | 0.4 / 152 / 217 |
| health start lateness, p50 / p99 / max | 58 / 282 / 509 | 32 / 180 / 1084 | 53 / 280 / 339 | 41 / 171 / 300 | 32 / 292 / 958 | 53 / 252 / 385 |
| processing response from the sensor job's arrival | 120 / 502 / 680 | 93 / 909 / 1259 | 97 / 586 / 2256 | 125 / 493 / 1929 | 95 / 987 / 3773 | 102 / 632 / 1456 |
| BMS_PackStatus, sample → transmit done | 362 / 678 / 911 | 338 / 971 / 2356 | 360 / 925 / 2054 | 370 / 700 / 1482 | 344 / 827 / 2432 | 366 / 909 / 1130 |
| BMS_PackStatus, sample → observer | 386 / 807 / 960 | 358 / 1003 / 2496 | 383 / 979 / 3093 | 404 / 811 / 1600 | 365 / 881 / 2448 | 392 / 954 / 1177 |
| BMS_Heartbeat period deviation, p50 / p99.9 / min / max | −2.7 / 485 / −2020 / 1908 | −0.7 / 888 / −1846 / 15270 | −2.3 / 748 / −3239 / 10283 | −0.3 / 497 / −2655 / 2212 | −0.6 / 941 / −3377 / 3465 | −1.1 / 793 / −1356 / 1352 |

The medians stay within 32 to 45 µs (sensor release lateness) and 338 to 370 µs (BMS_PackStatus
to the bus) in all six. The worst cases of the sensor release lateness, the processing response
and BMS_PackStatus are single jobs and frames under the flood and the download, and over UDP also
on the idle bus: the sensor task's release lateness reached 2.16 ms (SocketCAN download), 1.86 ms
(UDP idle) and 3.48 ms (UDP flood), the processing task's response 3.77 ms (UDP flood), and
BMS_PackStatus reached the bus at most 2.43 ms (UDP flood) and the observer at most 3.09 ms
(SocketCAN download) after its sample. The other protected
messages, sample → transmit done (`transports.<transport>.<condition>.latency`): p50 810 to
842 µs and at most 2.98 ms (BMS_CellVoltages1, on the SocketCAN idle bus), p50 1.04 to 1.07 ms
and at most 3.70 ms (BMS_CellVoltages2), p50 1.28 to 1.31 ms and at most 3.95 ms
(BMS_Temperatures).

No job was released more than a tick early (`released_more_than_a_tick_early` 0 for both tasks
in every condition), and over SocketCAN none before its tick was due. Over UDP the flood run
released one sensor job before its due time, 763 µs early, and the three download runs one
sensor job, 776 µs early, and one health job, 918 µs early (`release.<task>.released_before_due`
and the minima of `release_lateness`); the processing jobs those two samples released have the
negative minima of the processing response, −616 and −724 µs. The trace saw the tick count
ahead of the grid 7 to 27 times per condition, never more than a period ahead
(`tick_lead.seen_more_than_a_tick_before_due` 0), and the longest stretch ahead was 0.885 to
0.955 ms per condition (`tick_lead.longest_stretch_ahead`), against the limit of 50 periods.

posix_tick.c made up 27 to 160 ticks per condition and gave 7 to 27 of them back
(`runs[].tick_catch_ups`, `tick_give_backs`). The host made the node give up ticks only over
SocketCAN: 15 in the flood run, and 16 and 6 in two of the three download runs
(`runs[].tick_dropped`); none in any UDP run or any idle or control run. The lateness is measured
from the grid as posix_tick.c shifted it after a drop, so a drop shows in the periods instead:
the sensor task's longest period was 15.1 ms over its 10 ms in the SocketCAN flood run and
10.1 ms over in the SocketCAN downloads (`release.sensor.period_deviation`), and BMS_Heartbeat
had one interval of a cycle and a half or longer in that flood run and three in those downloads
(`periods.BMS_Heartbeat.a_cycle_and_a_half_or_longer`; the 15.3 ms and 10.3 ms maxima of the
table). Over UDP no interval of any cyclic message was that long.

Every run passed the refusal rules above: no sample lost, no publish missing on the bus
(`publishes_missing_on_the_bus` 0), no overrun, no trace record dropped, and every observer saw
exactly the node's frames. The wall clock stepped 2 to 4 times per condition, by up to 1.83 s
(UDP flood; `runs[].wall_clock_steps`, `largest_wall_clock_step_us`); 0 to 3 frames of the
protected messages and 0 to 14 intervals of the cyclic ones per condition had untrusted times
and were left out of the timing figures (`frames_time_not_trusted`,
`intervals_time_not_trusted`).

One trace record cost 18.8 to 22.6 ns (`runs[].trace_event_cost_ns`) at 2,415 to 10,807 events a
second, under 0.1 % of the time in every run (`trace_time_share`, a lower bound). The control
runs without the trace are no better: an idle share of 97.1 % on both transports against 97.1 %
and 97.0 % with the trace, and a BMS_Heartbeat period deviation p99 of 317 µs (SocketCAN) and
345 µs (UDP) against 314 µs and 284 µs with it (`transports.<transport>.control`).

The idle task had 97.1 % (SocketCAN) and 97.0 % (UDP) of the run time on the idle bus, 92.7 % and
92.0 % under the flood, where can_rx took 4.8 % and 5.5 %, and 90.1 % and 90.0 % during the
downloads, where diag took 3.0 % and can_rx 3.2 % and 3.3 % (`cpu.idle_share`,
`cpu.tasks.<task>.share`). The run time per job, x86-64 host time, next to the task table's
design budget (`cpu.tasks.<task>.run_time_per_job_us`): sensor 29 to 41 µs (100 µs), processing
31 to 37 µs (160 µs), health 631 to 665 µs (5,466 µs). Single jobs ran up to 7.83 ms (can_rx,
SocketCAN download, `execution_per_job.max`) in FreeRTOS's view, which includes any time the
host took the thread away.

The host stack depth, the maximum over every run (`stack_max`, `own_bytes` against
`size_bytes`): health 4,648 bytes against the 4,096 it was created with (in the SocketCAN
download and the UDP flood; 2,824 in the other conditions), IDLE 2,072 against 2,048 in every
condition, flash 3,752 of 4,096, and the other tasks 2,248 to 2,536 of 4,096. These are host
threads with x86-64 frames and the port's signal frames (above), not an overflow of the target;
phase 7 measures the target's.

**Command.** `python bench/node_timing.py`; `--transport`, `--seconds`, `--downloads`,
`--control-seconds`, `--flood-load` and `--sensor-seed` change the plan, and each is recorded in
`config`.

## bus_load: the load on the bus

**What it measures.** Every frame on the bus over at least 60 s of an idle bus (the node alone
with the observer) and over one download, on SocketCAN and on the UDP bus. For each identifier:
frames per second, the exact length of every frame in bits with its stuff bits, bits per second,
and its share of the 500 kbit/s bus. Also the total load; the node's frames, the tester's
requests (0x7E0) and the node's diagnostic responses (0x7E8) apart; and the DBC's nominal load
next to what was measured.

**How.** The [same observer](#the-clocks-of-a-run) captures every frame. Each frame's length
comes from [fcn_tools.canbits](../tools/fcn_tools/canbits.py), the independent reference of
lib/can/can_bits (held against it on more than 100,000 frames), applied to the frame's
identifier, format, DLC and data.

- The load counts the 3 bits of intermission after every frame, so that a full bus is 100 %;
  the figures without it are given as well.
- Frames per second are the frames in the window over its length, exact to one frame at each
  edge. The window of the idle run starts 1 s after the node's first heartbeat and ends at its
  last; that of a download runs from the tester's first request frame to the node's last
  response frame. Only frames whose times are trusted mark the edges (one time across a step of
  the wall clock can be a second off, and the window's length with it, see node_timing); a frame
  whose time is not trusted is placed in or out of the window by the time of its read.
- The DBC's nominal load of each cyclic message is its length plus the intermission, times 1000
  over its cycle time in ms, with two lengths: the worst-case stuffed length any content can
  need, and a typical length, the mean of 2,000 frames of its identifier and DLC with random data
  from a fixed seed.

**Checks against the node's own counters.** The frames of the node that the observer captured
must equal the frames its controller model sent (`tx_frames`), or no result is written. The
model's busy time over the run (its own frames and those it received on its timeline) is given
next to the captured frames' share over the same span.

**UDP against SocketCAN.** The frames, their lengths and the loads are the same on both: the
content does not depend on the transport, and the node's frames are paced by its controller
model in both cases. What differs is how a frame reaches the observer: the vcan loopback, or one
of fifteen UDP datagrams. Neither transport is a wire: the tester's frames reach the observer
when they are sent, not when a bus would carry them, so their load is what was offered; the
node's model places them on its own timeline.

**What it says about a microcontroller.** The lengths, the nominal load and each message's share
are protocol data and hold for the same frames on a real bus. The frames per second depend on the
host keeping the node's time: a host that makes the node give up ticks leaves its 10 ms messages
short of 100 per second, and the result records the ticks given up.

**Results.** The final run is [results/bus_load.json](../results/bus_load.json), drawn in
[results/bus_load.png](../results/bus_load.png). The frames' lengths and the nominal load are
protocol data; the frames per second, and the loads with them, depend on when the host ran the
node's tasks (the result's `host_timing_note`).

- *Idle* (a window of 58.99 s): the node sent 327.0 frames a second, a load of 7.7 % of the
  500 kbit/s bus on both transports, 7.6 % without the intermission
  (`summary.<transport>.idle_load`, `idle_load_without_intermission`), against the DBC's nominal
  7.6 % with the typical lengths and 8.8 % with the worst case (`idle_nominal_load_typical`,
  `idle_nominal_load_worst_case`; protocol data). Each message came at its nominal rate to
  within the frame at each edge of the window (100.0 a second for BMS_PackStatus and
  BMS_Heartbeat, 50.0 for each cell message, 58 BMS_NodeInfo frames in the window), with mean
  lengths of 112.1 to 121.0 bits against typical lengths of 110.7 to 112.8 bits and a worst case
  of 132. That puts each message's load at 0.99 to 1.08 times its typical nominal load
  (`transports.<transport>.idle.nominal.<message>.load_ratio_to_typical`), and the total at 1.02
  times (derived: `idle_load` over `idle_nominal_load_typical`).
- *Download* (one per transport, windows of 23.28 s and 23.32 s): 47.7 % of the bus on both
  transports (`download_load`), of which the tester's requests (0x7E0) 37.2 % at 1,558 and
  1,555 frames a second (`download_tester_load`; what was offered, as above) and the node's
  frames 10.5 % (`download_node_load`), its diagnostic responses (0x7E8) 2.7 % of the bus among
  them (`download_node_diag_load`).
- *Checks.* The observer captured every frame the node's controller model sent, 19,620 of 19,620
  idle and 10,608 of 10,608 in the download on both transports (`node_counters.tx_frames`,
  `captured_node_frames`), and the 36,268 tester frames it captured are the 36,268 the node
  received. The model was busy 7.6 % of the idle run and 45.2 % of the download run
  (`idle_model_busy_share`, `node_counters.model_busy_share`), the captured frames' share
  without intermission over the same spans 7.6 % and 45.2 %
  (`node_counters.captured_share_without_intermission`). No run dropped a tick
  (`node_counters.tick_dropped` 0). The wall clock stepped twice in each idle run and once in
  the SocketCAN download; 3 and 2 frames of the idle runs and 1 of each download had untrusted
  times and were placed by the time of their read (`times_not_trusted`).

**Command.** `python bench/bus_load.py` (`--seconds` of at least 60, `--downloads`,
`--transport`).

## bus_off: the way into bus off and the recovery

**What it measures.** How the simulated CAN controller goes bus off under a fault, and how the
node gets back onto the bus once the fault is gone, at 500, 125 and 1000 kbit/s, for the two
faults that take the controller bus off in the model:

- `short`: CAN_H shorted to CAN_L (`can.fault short`). The bench polls `can.status` every 1 ms and
  removes the short (`can.fault none`) as soon as it sees the bus-off count rise.
- `tx_errors`: `can.inject tx 32`, 32 transmission attempts destroyed by bit errors; the
  disturbance ends by itself with the last of them, at the bus off.

A bus held dominant and a bus nobody acknowledges never take the node bus off (it ends error
passive; tests/system/test_can_faults.py), so the bench has no trials of them.

For each trial: the transmit error counter attempt by attempt; the times from the fault to error
passive and to bus off; the hold-off of the node's recovery policy; the recovery sequence (ISO
11898-1's 128 occurrences of 11 recessive bits) in bit times and in ms, next to the standard's
1408 bit times; the time from the removal of the fault to error active, and to the end of the
first frame the node transmitted after it; the BMS_Heartbeat values the node never sent; and the
DTCs CAN_BUS_OFF and CAN_ERROR_PASSIVE as BMS_Fault reported them. At the end of each bit rate a
short is held through 7 bus offs, which shows the hold-off doubling from 100 ms to its 1600 ms
limit.

**Conditions.** One node per bit rate, on the UDP transport by default (`--transport socketcan`
for vcan0), on an idle bus. The trials alternate between the faults after a seeded random wait
of 0 to 20 ms, so that faults fall at every phase of the node's 10 ms schedule. Each trial starts
with the node error active, both counters 0, at least 5.2 s after its last recovery (the policy's
stable time of 5 s, after which the hold-off is 100 ms again), and both bus-off DTCs healed in
the last BMS_Fault it sent.

**Where each time comes from.** Every time is CLOCK_MONOTONIC unless said otherwise.

1. *The controller's trace* (`can.trace`, host only:
   [platform/posix/posix_can_trace.h](../platform/posix/posix_can_trace.h)), recorded by the
   node's hardware thread, which runs the controller model. For every step into bus off and out
   of it, it keeps the model's time of the step and the start of the thread's pass that carried
   it out, the state, the error counters, the model's count of destroyed attempts and the number
   of frames the node has transmitted; for the first frame after a recovery its start of frame,
   when it entered its mailbox, and its end of frame. The model's times follow exactly from its
   rules and the times they start from, and a start is sometimes a host time (below, error
   passive to bus off); the pass times are the host's. Adding a record copies 64 bytes into a ring
   of 256 with no clock read, no system call and no lock; on a healthy bus nothing is recorded
   ([docs/can-simulation.md](can-simulation.md) gives its measured cost).
2. *The node's log*, stamped with the node's clock when each line is made, to the microsecond:
   the transmit task's "controller state 1 -> 2" (just after it took the bus-off event, which
   starts the hold-off timer), "bus off: requesting recovery" (just before it asks for the
   recovery) and "controller state 2 -> 0"; the health task's DTC lines.
3. *`can.status`* before and after each trial: the model's counts of destroyed attempts, bus offs
   and late re-synchronisations.
4. *An observer* in the bench's process, a socket on the same bus read by a thread of its own.
   Each frame gets the monotonic time at which `recv()` returned (the kernel's arrival plus the
   thread's wake-up) and the kernel's receive time (CLOCK_REALTIME). A kernel time is used only
   for the interval between two frames, and only where the monotonic times of the same frames
   agree within 1 ms, which leaves out an interval across a step of the wall clock. The first
   frame after each recovery is found by its number among the node's frames, which the trace
   gives, not by comparing clocks; that holds because the observer hears every frame from the
   node's start, which the run's counts check.
5. *The node's statistics* after an orderly stop.

**The intervals, and who fixes each.** The result labels every figure with its part: "model"
(the controller model by ISO 11898-1's rules alone: the same on any host and whatever frames the
node sends), "policy" (the node's recovery policy, on the FreeRTOS tick), "host" (when the host
ran a thread), "mixed" (a sum of them, among them the node's schedule: which frames it sends
when).

| Interval | From → to | Part |
|---|---|---|
| fault to bus off | the pass that applied the fault → bus off (model) | mixed: the wait for a frame to send, then 24 (short) or 32 (tx_errors) destroyed attempts |
| error passive to bus off | model times | mixed: 16 destroyed attempts of an error-passive transmitter (model): under a short 26 bit times apart whatever the frames, 416 bit times; under tx_errors each half the frame attempted (rounded down) plus 25 bit times, so the frames in the node's mailboxes (its schedule) set the length; and a start at a host time (below). Under a short the trials that no host time entered are "model" (`passive_to_bus_off_bits`) and the others "mixed" (`passive_to_bus_off_bits_not_exact`, their count and values); under tx_errors the figure is "mixed" (`passive_to_bus_off_bits`), with the trials that no host time entered apart (`passive_to_bus_off_bits_exact`) |
| bus off to driver | bus off (model) → the transmit task's log line | host |
| hold-off | the transmit task's two log lines | policy: 100 ms of app/can_io.c in whole ticks, plus the host's wake-up of the task |
| request to count | the request's log line → the pass that served it | host |
| recovery sequence | that pass → error active (model) | model: 128 × 11 bit times exactly, 2.816 ms at 500 kbit/s, 11.264 ms at 125 kbit/s, 1.408 ms at 1 Mbit/s; a trial that differs by a nanosecond is refused |
| report lags | model time → the pass that carried it out | host |
| active to first submit | error active (model) → the first frame's mailbox | host |
| first frame on the bus | mailbox → its end of frame | mixed: arbitration and the length (model) of whichever frame the node sent first (its schedule; its length is `first_frame_bits`), and a start a late pass moved (host) |
| removal to error active, removal to first frame | `can.fault none` (short) or the bus off (tx_errors) → model times | mixed |
| removal to frame heard | removal → the observer's thread | mixed, including the observer's wake-up |
| bus silence heard | the last frame before → the first after, by kernel times | mixed |
| DTC heal | error active (model) → the health task's log line that the DTC is no longer active | mixed |

The recovery sequence equals the standard's minimum by construction of the model on an idle bus,
where no frame shortens or lengthens the recessive runs: the figure shows that the model
implements rule 12 and that the host never stretches it, not how a real controller behaves. The
DTC heal counts 50 health cycles of 100 ms from the cycle that saw the bus off, which lies within
the hold-off, so a heal can come a little before 5 s after error active. The bench's own polling
of `can.status` (every 1 ms until the bus off, every 5 ms until error active) is served in passes
of the hardware thread, so the host figures include passes the bench caused; the model's figures
are untouched by them. The hold-off is measured between two log stamps of the transmit task; the
policy's timer starts a few microseconds before the first of them.

Error passive to bus off is a chain of the model's times, but two kinds of start in it are host
times ([lib/can/can_ctrl.c](../lib/can/can_ctrl.c)). A pass of the hardware thread more than a
frame length late starts the frame that was due at its own time, which `late_resyncs` counts,
over the whole trial: the count does not say where. A frame loaded into an idle bus starts when
it entered its mailbox, at the driver's time; between two destroyed attempts the mailboxes are
empty only after the transmit queue recalled them (a frame gone stale in a mailbox,
[lib/can/can_txq.h](../lib/can/can_txq.h)), and nothing counts such a start. The trace does not
record when each attempt started, nor which frame it was. So a trial counts as one that no host
time entered (`runs[].trials.<fault>.passive_to_bus_off_exact`) when `late_resyncs` is 0 over
the trial and the interval is a whole number of bit times to the nanosecond (a bit is 8000, 2000
or 1000 ns at the three bit rates). A frame loaded at a host time that falls on the bit grid by
chance passes both tests (at 1 Mbit/s about one such start in 1000 would): under a short it then
shows as more than 416 bit times and the trial is refused, under tx_errors it counts as exact.

**What makes a run fail, and what is repeated.** No result is written when a trial has more or
less than one bus off; a transmit error counter off ISO 11898-1's rules at any sample; error
passive other than at 128 or bus off other than at 256, after other than 24 or 32 destroyed
attempts; under a short, error passive to bus off other than 416 bit times in a trial that no
host time entered; a recovery other than 1408 bit times exactly, or with counters other than 0;
the driver's log lines out of order with the trace; an observer that did not hear the frame the
trace names; or any fault active other than the two of the bus. Per bit rate, no result unless the
controller's bus-off events, the queue's bus-off count and its recovery requests equal the bus
offs the bench caused; the hold-off is back to 100 ms at the end; no event frame was dropped or
expired; the queue's accounting holds; both DTCs end with status 0x2E and one "active" and one
"no longer active" line per episode; the frames heard equal the frames sent; the BMS_Heartbeat
values missing equal those the queue superseded; and the persisting short shows 24 attempts and
1408 bit times at every bus off and N − 1 recoveries under the short. The verification of phase 6
built a scratch copy whose controller model recovers after 127 occurrences instead of 128, and
the bench refused its first trial (a recovery of 2.794 ms, not 1408 bit times) with nothing
written.

The host can make a trial unusable, and the bench repeats it: a short removed only after the node
had already asked to recover (the bench was held up for most of the 100 ms), or a hold-off above
141 ms (it looks doubled because the host held the transmit task up). Repeats are listed
(`repeated_trials`) and limited to 10 % of the trials + 5 per bit rate. Whether the first
heartbeat after a recovery was fresh, and whether the first BMS_Fault after it already carried
the DTC bits, depend on when the host ran the node's tasks: they are counts in the result, never
failures.

**Trial counts.** 100 trials of each fault at each bit rate, 600 in all, plus three persisting
shorts. The model's figures (the recovery sequence, the destroyed attempts to bus off, the TEC
at error passive and at bus off, and error passive to bus off under a short in the trials that
no host time entered) are constants, and no result is written when one differs in a trial: one
trial shows the value, and the others show that it does not vary (the minimum equals the
maximum). The figures that depend on the frames the node sends, error passive to bus off under
tx_errors and the first frame on the bus, vary from trial to trial and are "mixed". For the host
and mixed figures, n = 100 per fault and bit rate gives p50 and p90; p99 would need 500 trials
(about 4.5 hours). Every trial's values are kept in the JSON (`runs[].trials`). Almost all of a
trial's time is the stable time the policy needs before the next trial can start with the
initial hold-off.

**What it says about a microcontroller.** The way into bus off (24 or 32 attempts, TEC 128 and
256) and the 1408 bit times of the recovery are ISO 11898-1's; a controller that follows the
standard behaves the same on an idle bus. On a real bus, traffic during the recovery would change
the count (a frame passing contributes one occurrence); this bench measures an idle bus only.
The hold-off and its doubling are the node's policy and would be the same on a microcontroller,
timed by its tick. The host parts are wake-ups of Linux threads and say nothing about a
microcontroller, where the driver sees the bus-off interrupt within microseconds. The heartbeat
values lost follow from the time off the bus (mostly the 100 ms hold-off) and the 10 ms period.

**Results.** The final run is [results/bus_off.json](../results/bus_off.json), drawn in
[results/bus_off.png](../results/bus_off.png), on the UDP transport; each figure carries its part
in the result. No trial was repeated (`summary.<bit rate>.repeated_trials` 0), and every check of
every bit rate held (38 each, `runs[].checks`), among them the 207 bus offs caused per bit rate
against the controller's events, the queue's bus-off count and its recovery requests, and the
frames heard against the frames sent.

- *Standard and model.* The recovery sequence was 1408 bit times in every trial, 200 per bit
  rate with the minimum equal to the maximum: 2.816 ms at 500 kbit/s, 11.264 ms at 125 kbit/s and
  1.408 ms at 1 Mbit/s, the standard's minimum (`recovery_sequence_ms`, "model";
  `standard_minimum_recovery_ms`, "standard"). Every trial went error passive at a TEC of 128
  and bus off at 256, after 24 destroyed attempts under a short and 32 under tx_errors
  (`destroyed_attempts_to_bus_off`; `runs[].trials.<fault>.tec_at_passive`, `tec_at_bus_off`).
  Error passive to bus off under a short took 416 bit times in 298 of the 300 trials; the other
  two, 545 bit times at 500 kbit/s and 606 at 1 Mbit/s, each had a late pass counted
  (`runs[].trials.short.late_resyncs` 1). Under tx_errors it took 1280 to 1476 bit times
  (`passive_to_bus_off_bits`). The first frame after a recovery took 116 to 120 bit times on the
  bus (`first_frame_bits`).
- *Policy.* The hold-off was 100 ms plus 0.30 to 0.83 ms at the median of each fault and bit
  rate, and at most 1.14 ms more (`holdoff_ms`, "policy": 100 ms in whole ticks plus the host's
  wake-up of the task, as in the table above; derived: less the policy's 100 ms).
  The persisting short doubled it from 100 ms to the 1600 ms limit and held it there at every
  bit rate (`runs[].persisting_short.holdoffs_as_policy`), each of its 21 hold-offs 0.04 to
  1.03 ms above the policy's value (`persisting_short_holdoffs_ms`; derived).
- *Host.* Bus off to the transmit task's log line p50 49 to 62 µs, at most 410 µs
  (`host_bus_off_to_driver_ms`); the request to the pass that served it p50 25 to 28 µs, at most
  199 µs (`host_request_to_count_ms`); error active to the first frame in a mailbox p50 95 to
  117 µs, at most 607 µs (`host_active_to_first_submit_ms`).
- *Mixed.* Fault to bus off p50 4.75 to 9.52 ms at 500 kbit/s and 1 Mbit/s, 7.42 ms (short) and
  22.7 ms (tx_errors) at 125 kbit/s, at most 28.9 ms (`fault_to_bus_off_ms`). Bus off to error
  active p50 104 ms at 500 kbit/s, 112 ms at 125 kbit/s and 102 ms at 1 Mbit/s under both faults
  (`bus_off_to_error_active_ms`); removal of the fault to the end of the first frame p50 103 and
  104 ms, 112 and 113 ms, 101 and 102 ms (short and tx_errors), at most 114 ms
  (`removal_to_first_frame_ms`). The silence heard on the bus p50 112 to 140 ms
  (`bus_silence_heard_ms`), 0 to 6 trials per fault and bit rate left out where the two clocks
  disagreed by more than 1 ms.
- *Counts and the DTC heal.* 9 or 10 BMS_Heartbeat values lost per trial at 500 kbit/s and
  1 Mbit/s, 11 under a short and 12 or 13 under tx_errors at 125 kbit/s (`heartbeat_values_lost`,
  "mixed"). The first heartbeat after every recovery was fresh, and the first BMS_Fault after it
  already carried the DTCs, in 100 of 100 trials of each fault and bit rate
  (`first_heartbeat_fresh`, `dtc_reported_after_recovery`). The DTC heal took 4.94 to 4.96 s at
  the median and 4.90 to 5.01 s in all, 5 of the 600 at 5 s or more (`bus_off_dtc_heal_ms`,
  "mixed"; derived from `runs[].trials.<fault>.dtc_heal`).

**Command.** `python bench/bus_off.py`; `--bitrates`, `--faults`, `--trials`, `--persist`,
`--transport`, `--seed` and `--max-delay-ms` change the plan.

## isotp_throughput: the transfer rate of ISO-TP and UDS

**What it measures.** How fast the tester's data reach the node in TransferData requests, for
block sizes from 1 byte to the node's maximum of 1024 data bytes (maxNumberOfBlockLength 1026);
where each block's time goes; and how fast the node's longest response reaches the tester. The
node is the release build as it is; the tester is the flasher's, udsoncan 1.26.1 over can-isotp
2.0.7, configured by `fcn_tools.diag.connect()` (flow control BS 0, STmin 0, frames padded with
0xCC).

**How, tester to node.** For each block size N (default 1, 5, 6, 16, 64, 128, 256, 512, 768 and
1024 bytes), one run of fcn_node is started directly, without the bootloader, on a fresh flash
whose slot A holds a factory image (a data payload adopted as CONFIRMED), so that slot B can be
written. Once the node's first status frame is on the bus, the bench runs the flasher's
programming sequence into slot B in blocks of N bytes (`flasher.plan_blocks()`): the sessions,
eraseMemory, RequestDownload (whose answer must be maxNumberOfBlockLength 1026), an image of
max(64 N, 512) bytes (a 256-byte header and a payload of seeded random bytes) in TransferData
blocks, RequestTransferExit and the check routine. The image validates and is never started. The node is
then stopped in order and its statistics are read.

**The three clocks**, each used for what it can tell:

- *The tester's:* `time.monotonic()` in the bench's process around each `transfer_data()` call,
  as the flasher times a block, and at the start of each request. It includes udsoncan's and
  can-isotp's work in Python before the first frame and after the response.
- *The wire's:* a third participant on the bus (a python-can socket of its own on vcan0, or a
  port of its own on the UDP bus, read by a thread of the bench) records every frame with its
  kernel receive time, so that one clock stamps the frames of both sides. On SocketCAN a tester's
  frame is stamped when the tester's write reached vcan0, and a node's frame when the node's bus
  model wrote it (its modelled end of frame plus the host's pacing lag); on UDP when the datagram
  reached the listener. A tester's frame is not stamped when the node's model delivered it at 500
  kbit/s; that time is computed from bit counts instead. These stamps are CLOCK_REALTIME and are
  used only for differences within a block; a block is left out of the decomposition, and counted
  (`clock_disagreements`, by reason), when a stamp goes back within it, when its wire time from
  the first frame to the response exceeds the tester's own measurement by more than 0.5 ms, or
  when its wire period differs from the tester's by more than 5 ms. (A step of the wall clock of
  less than 5 ms inside a period would pass and distort that one block; the steps seen were about
  a second.)
- *The node's:* its statistics after the orderly stop, on CLOCK_MONOTONIC. The mean write job is
  `confirm.flash_task.jobs.write.total_ns / run` (`total_ns`, added in phase 6: the sum of the
  times the flash task measures around each job, `plat_time_ns()` before and after
  `flash_job_run()`, the waits for the flash included; one 64-bit addition per job inside a
  critical section the flash task already enters). Also the download's counters (bytes, blocks,
  `transfer_us` from RequestDownload to the RequestTransferExit response) and the diagnostic
  task's events and `budget_waits`.

**Each block, cut into turns.** From the wire, every TransferData request is cut into the turns
of the two sides, which add up exactly to its period (from its first frame to the first frame of
the tester's next request): the node's flow controls (from the tester's frame that completed a
block of consecutive frames, or the first frame, to the node's flow control); the tester's bursts
(from a flow control to the last consecutive frame it releases); the node's last turn (from the
request's last frame to the response: the frames still to take, the write job, the response);
and the tester's turnaround (from the response to its next request). A turn is the time between
two frames on the wire, not the time a side is busy: while the tester sends a burst, the node
already takes the frames that have arrived.

**Computed per block, not measured.** The frames (a first frame and ceil((N + 2 − 6) / 7)
consecutive frames, or one single frame for N ≤ 5); the node's flow controls (one after the first
frame, then one after every 16th consecutive frame but the last); the diagnostic task's events,
E = frames + flow controls; the event floor of the mean period, (E + 1) / 2 ticks of 1 ms (the
task takes at most 2 events a tick, and each block also brings the transmit-done report of its
response: for 1024 bytes 147 frames, 10 flow controls, 157 events and 79 ticks, the floor
[docs/diagnostics.md](diagnostics.md) names; a single block can take less, its events falling
into fewer ticks of its own); and the bus time of the block's frames, flow controls and response
from their exact bit counts at 500 kbit/s. The node's telemetry stays on, and its bus time within
each block is reported too.

**Showing what sets the floor.** For every block size the mean period less the mean write job is
compared with the event floor; a straight line is fitted to the sizes of 6 bytes and more
(period − write job = intercept + slope × events), whose slope is compared with 0.5 ms per event,
the budget of 2 events per 1 ms tick; and the node's `budget_waits` per block count the ticks the
task waited with events left.

**The projection for blocks of 2048 bytes, which is not a measurement.** The node is not rebuilt
with another maxNumberOfBlockLength (phase 6, decision 2: the node as it is), so 2048-byte blocks
are never measured. The bench computes from the measured parts the line at the events of a
2048-byte block (293 frames and 19 flow controls, 312 events) plus a write job of 8 pages at the
measured write time per page of the 1024-byte blocks, and the saving for a 256 KiB image (128
blocks against 256) from the line itself. Every projected figure says `"projection": true`. It
assumes that a 2048-byte block costs what the line says of its events, that the write time scales
with the pages, and that the tester and the host behave as for the measured sizes; it leaves out
what a receive buffer of 2050 bytes would cost the node in RAM.

**How, node to tester.** On one more run of the node (no flash), `--responses` (500) requests
`19 0A` (ReadDTCInformation reportSupportedDTC, the node's longest response: 71 bytes with its 17
DTCs, a first frame and 10 consecutive frames) through `fcn_tools.tester.Uds`, each timed by the
tester. Each response is cut on the wire into the request to the node's first frame, the first
frame to the tester's flow control, the flow control to the node's last consecutive frame, and
the gaps between consecutive frames (the node hands out a consecutive frame only after the
transmit-done report of the one before, an event of the diagnostic task). Responses whose stamps
go back, or whose wire time exceeds the tester's own by more than 0.5 ms, are counted and left
out.

**Conditions and counts.** Both transports, one bench at a time on an idle machine, the node's
telemetry on the bus and nothing else (a flood during a download is in node_timing and bus_load,
and in test_download's timing test). 64 blocks per size (512 for 1 byte, 103 for 5, 86 for 6, as
an image has at least 512 bytes) give the medians and 90th percentiles of the periods, not the
99th; 500 responses per transport give the 99th percentile of the response time.

**What it says about a microcontroller.** The counts, the floor and the line's slope are the
node's own design: the same tick and the same budget of 2 events per tick set the same floor on a
Cortex-M3, where the diagnostic task's work per event is short against a tick. The bus times are
those of a real 500 kbit/s bus for these frames. What belongs to the host: the latency of its
scheduler at each tick boundary; the write job's waits (each 0.4 ms page program ends at the next
tick, because the platform's wait hook polls once per tick, so a 1024-byte write job takes about
4 ms on the host against 1.6 ms of programming); and the tester in Python, whose turnaround and
bursts a tester on a CAN adapter would not have. The node's processor time per event is not
measured here.

**Results.** The final run is [results/isotp_throughput.json](../results/isotp_throughput.json),
drawn in [results/isotp_throughput.png](../results/isotp_throughput.png). The periods, write jobs
and turnarounds are host timing; the event floors and bus times are computed, and the
projection is marked as one.

- *Blocks of 1024 bytes:* period p50 82.8 ms over SocketCAN and 82.9 ms over UDP
  (`summary.<transport>.block_period`), 12,378 and 12,358 bytes/s (`transfer_throughput`),
  against the event floor of 79.0 ms (157 events) and 36.8 ms of bus time (`block_event_floor`,
  `block_bus_time`); the write job 3.97 and 3.96 ms on average (`block_write_job_mean`), the
  tester's turnaround 0.50 and 0.51 ms (`block_turnaround_mean`).
- *The floor.* At every block size the mean period less the mean write job was within 0.40 ms of
  the event floor (derived from `transports.<transport>.transfer[]`: `wire.period_ms.mean`,
  `write_job_ms.mean`, `event_floor_ms`). The line fitted to the 8 sizes of 6 bytes and more has
  a slope of 0.499 ms per event (SocketCAN) and 0.500 ms per event (UDP) against the budget's
  0.5 ms, intercepts of 0.45 and 0.38 ms, and r² of 0.99997 and 0.99999
  (`projection_2048.fit`). The diagnostic task waited with events left 76.5 and 76.0 ticks per
  1024-byte block (`budget_waits_per_block`). A block of 1 byte took p50 0.976 and 0.974 ms,
  against its floor of one tick.
- *Checks.* One block (of the 64 of 768 bytes over UDP) was left out of the decomposition, its
  stamps disagreeing with the tester's (`clock_disagreements`); every other full-size block was
  cut into its turns (`full_blocks_decomposed`). No stray frame and no response pending in any
  run.
- *The longest response* (71 bytes, 11 frames): wire time p50 6.41 ms (SocketCAN) and 6.43 ms
  (UDP), p99 7.71 and 7.70 ms, at most 8.25 and 7.84 ms (`longest_response_time`), 11,082 and
  11,050 bytes/s at the median (`longest_response_throughput`), against 3.14 ms of bus time; the
  gap between consecutive frames p50 0.425 and 0.415 ms; no response left out. The tester's own
  time for one response over SocketCAN was 23.6 ms (`response.tester_ms.max`), where the wire's
  longest was 8.25 ms: time in the tester's Python, not on the bus.
- *The projection* (`"projection": true`, not a measurement): blocks of 2048 bytes would carry
  12,482 bytes/s over SocketCAN and 12,469 over UDP, and save 0.186 s (0.9 %) and 0.176 s
  (0.8 %) of a 256 KiB image's transfer (`projected_2048_throughput`,
  `projected_256KiB_saving`, `projected_256KiB_saving_share`).

**Command.** `python bench/isotp_throughput.py`; `--transports`, `--block-sizes`, `--blocks` and
`--responses` change the plan.

## updates: end to end, and the 256 KiB image

**What it measures.** Updates of the running node over the bus with the flasher, 50 over
SocketCAN and 10 over the UDP bus, each from the image the one before confirmed: every step's
time, the total, and whether it succeeded. And the download of an image whose payload is exactly
256 KiB (262,144 bytes), 10 times: the erase, the transfer, the check, the total and the
throughput.

**The images.** They are made from the release node (checked to carry the tree's build id, also
when `FCN_NODE` names another node), stripped (`strip -o`) and packed with
[fcn_tools.image](../tools/fcn_tools/image.py): the factory image 1.0.0 in slot A of a new flash
(`fcn_flash factory`), the updates 2.n.0 (SocketCAN) and 3.n.0 (UDP), n the update's number.

**Each update**, one after the other: the board is started through fcn_boot
(`fcn_tools.node.Board`) with the product's options, so the trial's own window of 5 s of good
health stays in force. Once its first status frame is on the bus, `python -m fcn_tools.flasher
--can <bus> --report <file> <image>` makes the whole update into the slot that does not run (B,
then A, B, ...): boot_info, the sessions, erase, request_download, transfer, transfer_exit, check,
ecu_reset, wait_for_trial, read_version, extended_session_trial, confirm and
boot_info_confirmed. The Board starts the trial through fcn_boot after the ECUReset. The bench
then stops the board in order, reads the trial run's statistics, reads the flash both ways
(`fcn_flash info` and fcn_tools.flash must agree) and checks the reference's invariants, so the
next update begins from the image this one confirmed.

**The times.** Each step's time is the flasher's: `time.monotonic()` around the step in the
flasher's process, so it includes the bus and both hosts. The total is the sum of the steps: the
transfer and the erase make most of it, and between the check and read_version there are the
flasher's ecu_reset and wait_for_trial. wait_for_trial is the flasher's poll for the trial
([fcn_tools.flasher](../tools/fcn_tools/flasher.py), `_wait_for_trial()`): its first `22 FD 00`
50 ms after the ECUReset's answer, each request waiting the node's P2server of 50 ms, the next
50 ms after one that did not find the trial, and, after a request that went unanswered, the
wait for an answer still due (50 ms when none is). It is therefore the node's restart rounded up
to that poll, normally the first interval and one request, and not the restart itself. The
flasher's wall time adds its Python start-up; the time from the board's start to its first
status frame is the bench's, from before it starts fcn_boot to when its own listener has read
the frame, so it includes fcn_boot's start, the node's start-up and the bench's own wake-up.

**The node's restart** (`reset_to_trial`) is the node's own time, from its log: from the
download run's line `ECUReset: resetting (requested by the tester)`, written just before
`plat_reset()`, to the trial run's line `started by the bootloader: slot <slot> as a trial, ...`,
written at its start-up before the scheduler. Both are lib/log's stamps of the node's
CLOCK_MONOTONIC, in microseconds, and both runs are on the same host, so the two are subtracted as
they are. The interval holds the old run's end, the Board starting fcn_boot again (as a reset
into the bootloader would), fcn_boot itself (it validates the trial, counts its attempt in the
flash, copies the payload into a memory file checking its digest, and execs it) and the trial's
start-up up to its hand-off; it ends before the trial's tasks run, so its first status frame and
its first answer come later. A successful update whose logs lack either line, or have the
trial's before the reset's, is left out of the figure and counted
(`summary.updates_<transport>.reset_to_trial_refused`).

**When an update succeeds.** The flasher exits 0 with result `confirmed`; the download's run
ended by the requested reset (status 10) and the trial's run in order (status 0); the download
run's log has the erase of the image's size in 1 + ceil(size / 4096) steps, the check in 8 +
ceil(payload / 64) + 1 steps, and the reset for the tester; the trial run's statistics have the
hand-off of the target slot as a trial, attempt 1, of the new version, and the confirm state
CONFIRMED; and the flash holds the new version in the target slot. Who confirmed (the routine, or
the node itself if its 5 s came first) is recorded and counted. A run that ends in a reset writes
no statistics by design, so the download run's account of the download is its log; the node's own
count of blocks and write jobs comes from the 256 KiB downloads below.

**A failed update** is kept with the flasher's whole report, the exit status of each run and the
last lines of its stderr, and is never repeated. The campaign goes on from whatever the flash then
holds if an update can begin there (an image confirmed, no trial pending), and stops otherwise.
The summary gives the requested, attempted and succeeded counts, the distributions are those of
the successful updates, and a result that holds a failure ends with exit status 5, so that the
final runs stop there until someone has looked at it. The verification of phase 6 planted a
flasher that corrupts the image and saw the update and the download counted as failed, with the
failed step, and the status 5.

**The 256 KiB image.** The payload is the stripped release node padded with zero bytes to 262,144
bytes, so the image (262,400 bytes with its header) is a program. Each download uses a fresh flash
with the factory image in slot A and a node started directly (no bootloader), and `python -m
fcn_tools.flasher --address 0x102000 --no-reset`: the steps up to the check, which commits the
image as the trial. The node is then stopped in order, so its statistics count the download: the
bytes, 257 blocks and 257 write jobs with their mean time, the erase job (66 steps) and the check
job (4105 steps), `transfer_us`; all of them are checked. Two throughputs are given, the
flasher's (bytes over the transfer step) and the node's (bytes received over `transfer_us`). After
the last download the board is started once through fcn_boot on that flash, and its statistics
show the image starting as the trial in slot B, attempt 1.

**Conditions and counts.** One bench at a time on an idle machine; SocketCAN first, then UDP; the
node's telemetry on; the 256 KiB downloads over SocketCAN. 50 updates over SocketCAN give the
medians and the 90th percentiles of each step, 10 updates over UDP and 10 downloads the medians
only. The success count is out of 50 and 10, as the brief asks; it is no failure rate to quote
to a part per thousand.

**What it says about a microcontroller.** The steps, their order and the counts (bytes, blocks,
steps, slots, versions) are the node's own and would be the same on a target. So is every
success: a stall of the host past the node's watchdog (500 ms) or the ISO-TP timeout (1 s) can
fail an update, and the bench counts it and keeps its record, but nothing the host does makes
one succeed, so the updates counted as succeeded hold whatever the host did.
The times are host times: the transfer is set by the diagnostic task's budget of 2 events per 1 ms
tick, which a target with the same tick keeps; the erase by the modelled flash (45 ms per 4 KiB
sector, each wait ending at a tick); the node's restart (`reset_to_trial`) is the end of a host
process, fcn_boot as a host process started again by the Board (memfd and exec, not a jump) and
the trial's start-up on the host; wait_for_trial is the tester's poll for the trial, a choice of
the flasher's and no time of the node's; and the tester's Python adds its turnaround to every
request.

**Results.** The final run is [results/updates.json](../results/updates.json), drawn in
[results/updates.png](../results/updates.png). The counts and the successes do not depend on the
host; the times are host timing.

- *Updates.* All 50 over SocketCAN and all 10 over UDP succeeded
  (`summary.updates_<transport>.succeeded`), versions 2.1.0 to 2.50.0 and 3.1.0 to 3.10.0 into
  slots B and A in turn, each confirmed by the routine
  (`campaigns.<transport>.figures.confirmed_by`: the routine 50 and 10, the node 0). The image
  was 252,560 bytes in 247 blocks. Every download
  run ended with status 10 and every trial run with 0, every download log had the erase in 63
  steps and the check in 3952, and every trial started in its target slot as a trial, attempt 1,
  and ended CONFIRMED (`campaigns.<transport>.records`).
- *Times over SocketCAN* (p50, p90, max; `summary.updates_socketcan`): total 24.5, 24.5 and
  24.6 s; erase 2.85, 2.85 and 2.89 s; transfer 20.4, 20.5 and 20.6 s; check 4, 5 and 49 ms;
  the reset and the start of the trial (`reset_to_trial`) 1.20 s in all three; confirm 3, 4 and
  49 ms. The transfer ran at 12,378 bytes/s at the median (`transfer_throughput`).
- *Times over UDP* (p50 and max; `summary.updates_udp`): total 24.5 and 24.6 s, erase 2.84 and
  2.85 s, transfer 20.5 and 20.5 s, check 4 and 5 ms, reset to trial 1.20 and 1.20 s, confirm
  3 and 4 ms, the transfer at 12,350 bytes/s at the median.
- *Around the steps:* the flasher's wall time, with its start-up, p50 24.8 s (SocketCAN) and
  24.9 s (UDP); the board's start to its first status frame 14 ms at the median on both
  (`campaigns.<transport>.figures.flasher_wall_s`, `start_to_ready_s`).
- *The 256 KiB image* (262,400 bytes): all 10 downloads committed
  (`summary.big_image.succeeded`), each with 257 blocks and 257 write jobs, the erase job in 66
  steps and the check job in 4105 (`big_image.records`); erase p50 2.98 s, transfer 21.2 s,
  check 4 ms, total 24.2 s; 12,375 bytes/s by the flasher's count and 12,374 bytes/s by the
  node's at the median (`summary.big_image.transfer_throughput`,
  `big_image.figures.node_transfer_bytes_per_s`), the mean write job p50 4.00 ms
  (`big_image.figures.write_job_mean_ms`). Started through fcn_boot afterwards, the image ran as
  the trial in slot B, attempt 1 (`big_image.booted_afterwards`).

**Command.** `python bench/updates.py`; `--socketcan-updates`, `--udp-updates`, `--big-downloads`,
`--big-payload` and `--big-transport` change the plan.

## power_loss: the power-loss campaign

**What it measures.** Whether a loss of supply in the middle of an update ever leaves the board in
a state the design does not allow, or unable to boot and be updated again, over at least 1000
cuts made on the real programs: fcn_flash, fcn_boot, and the node's flash task under the FreeRTOS
scheduler, serving a tester's download over the bus. The figures are counts: trials per class and
per operation; what each cut left (the state before the operation or after it, and how much of
the torn operation was done: none, some or all of its transitions); which state the board came
back in and what its first start did; and how many trials failed a check, each expected to be 0.
It also records the wall time of each trial and of the campaign, CLOCK_MONOTONIC in the process
that ran it, while the trials run in parallel: host figures.

**The trials.** Each trial copies a base flash, runs one sequence of the update with the supply cut
in one of its flash operations, starts the board again through fcn_boot as after a loss of
supply, and makes the next update. Three classes:

- **stage**: `fcn_flash stage` of a 9256-byte image, 44 operations: the BEGIN record (a 96-byte
  record program and an 8-byte commit-field program), 3 sector erases, 37 page programs (the last
  of 40 bytes) and the COMMIT record. The node's own image (252,560 bytes in the final run,
  `images` in the result) would make 1053 (2 + 62 erases + 987 pages + 2), too many to cut each
  several times.
- **download**: the same 44 operations, made by the node's flash task for a tester's download.
  The check routine's validation only reads, so a cut "in the validation" is a cut at the start of
  the COMMIT record's first program.
- **boot**: the bootloader's attempt and the node's CONFIRM record of a trial that runs and
  confirms itself after the product's 5 s window; and the three attempts and the bootloader's
  ROLLBACK record of a trial that crashes at every start.

Each class also runs at log positions where its record is the first of a metadata sector, and so
first erases a sector that still holds 32 older valid records (the log filled by a history of
crashing trials rolled back). Each operation of a sequence (a cell) is cut a fixed number of times,
each with its own seed drawn from the plan's seed: 8 for stage (50 cells), 4 for download (50
cells), 33 for boot (14 cells), 1062 trials, in an order shuffled from the seed (20261123). Every
cell is cut exactly that many times by construction (`fcn_tools.powerloss.make_plan()`), and the
result's rows show it. Not in the campaign: the adoption of a factory flash, which the reference
does not cover (lib/boot's unit campaign does), and REJECT and FALLBACK, which need a damaged
image (test_update.py has them).

**How a cut is made, and why the parallel workers change no outcome.** Every cut is armed on a
command line: `--flash-cut N:SEED` of fcn_flash, of fcn_boot, or of the node after fcn_boot's
`--`. N is the operation's number in that program's own run, and the tear is drawn from the seed by
lib/flash's SplitMix64. What a cut leaves is a function of the base flash, the sequence, N and the
seed, never of the host's clock; no cut is armed at run time. The trials run in 8 worker processes,
each with its own flash file, retained cell, working directory and UDP ports; the images and bases
are only read. The host's timing can change when things happen, not what is written: a host that
holds the node up for longer than a health period restarts a trial's window of good health, and
the bench waits up to 30 s for the confirm. A stall longer than the node's watchdog (500 ms) or a
protocol timer (ISO-TP, 1 s) does fail a trial: the node's evidence goes into the trial's message,
and the trial is kept, never repeated. The only repeated start is one where fcn_boot finds the
flash or the cell still locked by the run before it (status 1, "in use"), which ends at its first
open and writes nothing; such starts are counted.

**The checks after every cut**, in this order; a failure of any is kept with the trial's seed and
its `--replay` spec:

1. The run ended as a power loss (status 12), and its FATAL line names the operation drawn: its
   number in the run, its kind, length and address, and the seed.
2. `fcn_flash info` and fcn_tools.flash read the flash the same.
3. `check_invariants()` holds.
4. The state (both slots' states, the newest record's number and event, the attempts used) is the
   one before the operation or the one after it, both written from docs/update.md's state machine
   in [fcn_tools.powerloss](../tools/fcn_tools/powerloss.py).
5. Every start after the cut, until a confirmed image runs, does what `expected_boot()` says and
   leaves the flash as the state machine says, checked from the image's own report: the node's
   hand-off and reset cause (POWER_ON after the cut), the crash payload's log line, or fcn_boot's
   exit 42 for an image that is no program.
6. A trial is started at most three times in all, counting the starts before the cut, and never
   more often than attempts were counted for it.
7. The next update succeeds and confirms: a staging of the node for stage and boot, and for
   download a download of the node with the flasher, which resets the node and confirms the trial.

Before the campaign each sequence also runs once without a cut: the operation counts of fcn_flash
or of the node and the state at the end must match the model, or the bench stops. The summary
separates the invariant violations (checks 2 to 6) from cuts not where drawn, failed next updates
and the bench's own errors. A result with a failed trial is written and the bench exits 5. The
verification of phase 6 planted three defects in a scratch copy and saw each flagged in a --quick
campaign: a cut that ends the run with status 0 instead of 12 (every trial failed check 1), a
reference that wants one attempt more for a trial (check 5), and a reference whose invariants
refuse any attempt used (check 3).

What the host can still do to a trial, the verification's rehearsal showed. At about a fifth of
the default (248 trials in 8 workers on an otherwise idle machine, the 1-minute load average 5.2
at the campaign's end from its own workers), one trial of the download class passed checks 1 to
6 and failed the next update: the node's hardware watchdog reset it during the flasher's
transfer ("not kicked for 501 ms"), and the flasher timed out. Its `--replay` alone, with the same
cut, tear and state, passed, next update included. The bench counts such a trial as a failed next
update (`follow_up_failures`, apart from the invariant violations), keeps it with its message and
replay, and exits 5: it cannot tell a host that stalled the node from a node that hung, and it
does not repeat a trial. A campaign of 200 download trials can therefore end with a few of them
(the final one had none); `--workers` fewer than 8 loads the host less and takes longer.

**Trial counts and why they are enough.** The boot operations get the most cuts because each of
them changes the metadata, and a one-unit program (an attempt, a commit field) has three outcomes,
all of which must occur; the result shows the tears per operation role. The campaign is not
exhaustive: lib/boot's unit campaign cuts every operation of every sequence at every position
(33,093 cuts). What this one adds is the real programs and files: the bootloader's starts and
hand-off, the node's flash task under the scheduler, the download over the bus, and an update after
every cut.

**What it does and does not say about a microcontroller.** What a cut leaves and the state the
board comes back in are decided by the flash model and the programs, not the host's timing, so
those counts hold for this code under lib/flash's tear model. That model has no weak or unstable
bits, no ECC, no erase suspend and no pre-programming; it tears a program in the order of its
units; and its flash is a file ([docs/update.md](update.md), the NOR model). Whether a trial
passes, the host can change in one direction only: a stall past the node's watchdog or a
protocol timer fails it (above), and nothing the host does makes a check pass, so a campaign
without failures holds whatever the host did, while a failure has to be replayed to tell the
host from the node. How often a start finds the files still locked by the run before it
(`lock_retries`) depends on the host's timing too. The times per trial are host times of parallel
processes and say nothing about a target.

**Results.** The final run is [results/power_loss.json](../results/power_loss.json), drawn in
[results/power_loss.png](../results/power_loss.png). Its counts do not depend on the host; only the
times per trial and the wall time do.

- *Trials.* 1062: 400 of stage, 200 of download and 462 of boot (`summary.trials_per_class`),
  every one of the 114 cells cut exactly 8, 4 or 33 times (derived from the rows of `trials`).
  Invariant violations 0, cuts not where drawn 0, failed next updates 0 and bench errors 0
  (`summary.invariant_violations`, `cuts_not_where_drawn`, `follow_up_failures`,
  `bench_errors`), and `failures` is empty: every trial passed checks 1 to 7, and every next
  update confirmed.
- *What the cuts left.* The state before the operation in 901 trials and the state after it in
  161 (stage 390 and 10, download 195 and 5, boot 316 and 146); a tear of all the transitions in
  378, of some in 299, of none in 331, and nothing to change in 54 (`trials`, columns `left` and
  `tear`; per role in `by_class.<class>.by_role`). Every role of the boot class saw all three
  tears; with 4 cuts per cell, the two commit-field programs of the download class saw two of
  the three each.
- *The first start after the cut* ran the confirmed image in slot A in 618 trials, rolled back
  to it (ROLLBACK) in 164, ran the trial in slot B in 251 (attempt 1 in 18, 2 in 199, 3 in 34),
  and ran the confirmed image in slot B in 29 (`trials`, column `boot`).
- *Locks.* No start found the flash or the cell still locked: `lock_retries` is 0 in all 1062
  rows of `trials`.
- *Times* (host, 8 workers in parallel): per trial p50 6.18 s (stage), 26.9 s (download) and
  6.31 s (boot), at most 6.53, 29.9 and 15.8 s (`summary.seconds_per_trial`); the whole campaign
  26.0 min (`summary.wall_time`, 1561.8 s), with the load average at 2.67 at its end
  (`provenance.host.load_average_at_end`).

**Command.** `python bench/power_loss.py`; `--workers`, `--seed`, `--stage-repeats`,
`--download-repeats` and `--boot-repeats` change the plan; `--replay CLASS:SEQUENCE:OP:SEED` runs
one trial of a result again and keeps its files.

## platform_timing: the host mechanisms

**What it measures.** The two host mechanisms the node's timing rests on, with the platform
self-test (tests/platform/plat_selftest, its own process):

1. *Tick accuracy.* The FreeRTOS POSIX port makes its tick with a relative sleep and so runs slow
   against the wall clock; platform/posix/posix_tick.c locks the tick count to CLOCK_MONOTONIC.
   Both are measured with a 10 ms periodic task: the effective tick period, the drift over the run,
   the catch-up, given-back and dropped ticks, the stalls of the whole process a witness thread
   saw, and the interval between the task's wake-ups.
2. *Simulated interrupt latency.* From a peripheral model raising an interrupt line to the task it
   wakes, for every raise (one every 250 µs, about the frame rate of a saturated 500 kbit/s bus),
   with signal delivery and with tick-hook polling (platform/posix/posix_irq.c), while a
   mid-priority task loads the scheduler.
3. *Interrupt storm.* The same line raised in a tight loop: every raise served, and handlers never
   stacked on a task's stack.

It builds the release preset first unless `FCN_SELFTEST` names a self-test; it writes a result only
if the self-test's build id is the tree's (exit 1 otherwise, as before phase 6). Phase 6 gave it the
load check of the other benches (exit 3, `--max-load`, `--allow-loaded`) and `--out`. Its summary
predates phase 6 (`percentiles()`, the same percentile rule, no standard deviation), and it keeps
its raw samples next to the build, not in the JSON. Its figures are those of this host's scheduler
and timers, which is exactly what they are meant to show; they say nothing about a microcontroller.

**Results.** The final run is [results/platform_timing.json](../results/platform_timing.json),
drawn in [results/platform_timing.png](../results/platform_timing.png): runs of 10 s, all host
timing.

- *Tick.* The stock port's tick ran 2.5 % long against CLOCK_MONOTONIC, an effective period of
  1025 µs, 250 ms behind the clock after the run, and the 10 ms task woke every 10.3 ms on
  average (`tick.stock`). The locked tick kept 1000 µs (an error of 0.0 %, a drift of −30 µs
  over the run), with 3 ticks made up and none dropped or given back, no stall of the process
  seen, and the 10 ms task's wake-ups 10.0 ms apart on average, 8.90 to 11.2 ms (`tick.locked`).
- *Interrupt latency*, every one of 40,021 raises in each mode: with signal delivery p50 31 µs,
  p99 136 µs, p99.9 282 µs, at most 773 µs, the line serviced 38,824 times by the signal handler
  and 1,166 times by the tick hook, the handler depth at most 1 (`interrupt.signal`); with
  tick-hook polling p50 534 µs, p99 1045 µs, p99.9 1539 µs, at most 2060 µs, about half a
  millisecond on average (532 µs) because the tick hook serves the line once a tick, in 9,986
  task wake-ups (`interrupt.poll`). No raise was left unserved.
- *Storm* (5 s each): with signals 75,976,581 raises, 15.2 million a second, and 1,475,166 task
  wake-ups; with polling 144,078,216 raises and 5,000 wake-ups, one a tick; none unserved, the
  handler depth at most 1 with signals and 0 with polling, the stack's peak 6,584 and 6,520
  bytes of a host thread (`interrupt_storm`).

**Command.** `python bench/platform_timing.py` (4 runs of `--seconds` 10 and 2 storms of 5 s).

## What the figures do not say

- **The task table's execution-time budgets stay design figures.** app/task_table.c's budgets are
  for a Cortex-M3 class target, and phase 6 has no target: it runs on the host. What it measures
  instead is the host run time per task, from FreeRTOS's run-time statistics and the node's trace
  (node_timing), which says nothing about a Cortex-M3. Phase 7 builds the node for a Cortex-M3
  under QEMU, which is not cycle-accurate either. "Schedulable" therefore still means: schedulable
  as long as no job runs longer than its budget ([docs/architecture.md](architecture.md#schedulability)).
- **A release can come before its tick was due.** As described under node_timing, a tick that
  posix_tick.c made up for one that was only late puts the kernel's tick count one tick ahead of
  CLOCK_MONOTONIC from the moment the late one is taken to the next tick boundary, where it is
  given back ([bug 011](bug-log.md#011-a-tick-made-up-for-a-late-one-kept-the-count-a-tick-ahead-of-the-clock)).
  A task released in that time is released less than a period before its tick was due;
  node_timing counts those jobs. Two late ticks taken in the same period put the count two
  ahead, and a task released at the second one to two periods early; node_timing counts those
  apart (`released_more_than_a_tick_early`) and gives the longest stretch with the count ahead
  of the grid. In the final run it counted one sensor job each in the UDP flood and download
  conditions and one health job in the UDP download, each less than a tick early, and none over
  SocketCAN. The platform's long-run tick period (platform_timing) is not affected.
- **Host stacks are not target stacks** (node_timing), **the recovery sequence is the model's**
  (bus_off), **the 2048-byte block is a projection** (isotp_throughput), and **the power-loss
  counts hold under lib/flash's tear model** (power_loss).
- **A failure can be the host's; a success is the node's.** A stall of the host past the node's
  watchdog (500 ms) or a protocol timer (ISO-TP, 1 s) resets the node or ends an exchange, and
  the update or trial fails: updates and power_loss count and keep it (exit status 5), the other
  benches then write no result. No stall makes a check pass, so a count of 0 failures, and every
  update counted as succeeded, holds whatever the host did. The counts of what happens in a
  stretch of time are the host's in the same way as its times: the jobs and frames of a run of
  fixed length, the ticks node_timing reports dropped and the jobs it reports released early,
  and power_loss's `lock_retries`.
- **The load check is blind to Windows.** The load average counts only the processes of the WSL
  VM; the final runs are made with the laptop otherwise idle.
- **The wall clock steps under WSL2**, which costs a few frames per run of the observers' timing
  figures, never silently (they are counted).
- **The VM loses wall time in short freezes** that its own clocks do not see, about 3 to 4 % in
  the check made before the final runs ([the clocks of a run](#the-clocks-of-a-run)); every
  figure here is on the VM's clock, and none corrects for them.

## The final runs

The lead runs every bench on the committed tree, alone on the machine, so that every result's
provenance names a commit and no other work shares the host:

1. Commit everything; nothing else runs (`uptime` shows a load average below 1.0, `ps` no leftover
   node, test run or busy loop); `scripts/vcan-up.sh` has made vcan0.
2. `python bench/run_all.py` (`--list` shows the plan). It refuses a tree with uncommitted changes,
   builds the release preset once, and runs the benches one at a time in this order, each once the
   load average is at or below 1.0 again (at most 15 minutes of waiting), stopping at the first
   bench that fails or reports a finding (`--from NAME` resumes there): node_timing, bus_load,
   bus_off, isotp_throughput, updates, power_loss, platform_timing. The benches whose figures are
   host timing run first; the power-loss campaign, which runs parallel workers and decides nothing
   from the host's clock, comes last before platform_timing, which closes the run.
3. Commit results/ with a message naming the commit the results were measured on.

[bench/README.md](../bench/README.md) gives each bench's expected duration, measured in the
verification's rehearsal and extrapolated to the default.

The final runs followed this procedure. bench/run_all.py ran the seven benches in the order above
on commit 28d8f7a with a clean tree, from 18:36:09 to 20:51:33 EST on 2026-11-23, and results/
was committed as 2d93e3e, whose message names 28d8f7a. run_all's own summary, each duration with
the bench's build and setup: node_timing 12.7 min, bus_load 3.0 min, bus_off 54.1 min,
isotp_throughput 1.0 min, updates 29.1 min, power_loss 26.1 min and platform_timing 0.9 min,
every one with exit status 0; before platform_timing it waited 60 s for the load average to
fall to 1.0 after the power-loss campaign. Windows logged no standby (System log, Kernel-Power events 506 and 507)
between 18:30 and the end of the run. Every result's provenance names commit 28d8f7a with no
modified files, every binary it measured carries that commit as its build id, and none is marked
loaded: the 1-minute load average at the benches' starts was 0.10 to 0.98
(`provenance.host.load_average_at_start`). No start of the board in the power-loss campaign
found the flash or the cell still locked by the run before it: `lock_retries`, the count of such
starts, is 0 in all 1062 rows of [results/power_loss.json](../results/power_loss.json); their
cause was fixed by
[bug 012](bug-log.md#012-the-next-start-found-the-files-still-locked-by-the-run-that-had-just-ended).
