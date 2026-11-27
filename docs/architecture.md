# The application

What the node's firmware does: its tasks and their priorities and periods, how they talk to each
other and why each mechanism was chosen, the path of a measurement from the front end to a frame
on the bus, and the analysis that says whether the tasks meet their deadlines. Phase 3 added the
fault monitor and the supervision of the tasks to the processing and the health task
([app/fault.h](../app/fault.h), SUPERVISION in [app/app.c](../app/app.c)). Phase 4 added the
node's side of an update: the bootloader's hand-off read at start, the confirm of a trial image,
and the flash task that makes every write of the node to the flash ([app/update.h](../app/update.h),
THE UPDATE in [app/app.c](../app/app.c)). Phase 5 added the diagnostic task, which serves a tester
on the bus with ISO-TP and UDS ([app/diag.h](../app/diag.h), lib/isotp and lib/uds), the flash
task's jobs of a download ([app/flash_job.h](../app/flash_job.h)), the supervision of the flash
task, and the snapshot and the clear of the DTCs for the UDS services (THE DIAGNOSTIC TASK and
THE FLASH TASK in [app/app.c](../app/app.c)).
The messages themselves are described in [can-protocol.md](can-protocol.md), the simulated bus in
[can-simulation.md](can-simulation.md) and the sensor model in [sensor-model.md](sensor-model.md).

```
  front end ──> sensor ──(sample queue)──> process ──> protected telemetry ─────────────┐
  (plat_sensor.h)  │                         │  │                                        │
                   └──> BMS_Heartbeat ───────┼──┼────────────────────────────────────────┤
                                             │  └──(verdict queue)──> health ──> BMS_Fault, task stats,
                                             v                          │  │     node info ──┤
                                 pack state (mutex) <───────────────────┘  │                 v
                                 DTC summary ──> process (PackFaultActive) │       can_io (can_tx) ──> bus
  bus ──> can_io (can_rx) ──> TESTER_Command check (command.c) ──> telemetry switch ──> process
                                                              └──> clear requests ──> health

  sensor, process, can_rx, can_tx ──(alive indications)──> watchdog manager (lib/wdgm), run by health
  health ──(kick, while the manager allows it)──> hardware watchdog (plat_wdg.h) ──> reset if not kicked

  bootloader's hand-off (plat_retained.h) ──> app_start: ActiveSlot, BMS_BootInfo, UPDATE_ROLLBACK
  health ──(confirm request, once a trial ran 5 s in good health)──> flash ──> lib/boot ──> flash (plat_flash.h)
         <──(the confirm's result)──────────────────────────────────┘

  bus ──> can_rx ──(UDS_Request 0x7E0, a queue of 18)──> diag: lib/isotp ──> lib/uds ──(backend)
  can_tx ──(reports of UDS_Response 0x7E8 sent, a queue of 8)──> diag       │
  diag ──(UDS_Response frames, event frames of the transmit queue)──> can_tx ──> bus
  diag ──(a flash job: erase, write, exit, check, confirm)──> flash ──(done)──> diag
  health ──(the DTCs' snapshot, the clears carried out, the confirm's state)──> diag
  diag ──(ClearDiagnosticInformation: one more clear request)──> health
```

The DTCs ([lib/dtc](../lib/dtc/dtc.h), the faults in [app/fault.c](../app/fault.c)) have one
owner, the health task; the processing task judges the samples and hands it the verdicts, and
every other task reads only the summary the health task publishes. The supervision cycle of the
watchdog manager and the kick of the hardware watchdog are the first thing the health task does
in every period. [faults.md](faults.md) describes the fault handling in full: the faults and
their DTCs, the fault frame, the watchdogs and the reset cause.

## Tasks

Seven tasks of the application, all created statically before the scheduler starts
([app/app.c](../app/app.c), [app/can_io.c](../app/can_io.c)); priorities and periods are defined in
one place, [app/app_tasks.h](../app/app_tasks.h) and [app/can_io.h](../app/can_io.h). A higher number
is more urgent.

| Task | Priority | Released by | Does |
|---|---|---|---|
| `can_rx` | 7 | the CAN interrupt (task notification); at the latest after 50 ms, and at the end of every 100 ms receive cycle | takes received frames out of the controller's FIFO (at most 32 per activation) and calls their handlers: a pong for every ping, the check of every TESTER_Command, every UDS_Request frame into the diagnostic task's queue; once per receive cycle the TESTER_Command check without data |
| `sensor` | 6 | every 10 ms (`xTaskDelayUntil`) | publishes BMS_Heartbeat, reads the current in every period, the cells in every second and the temperatures in every tenth, posts the sample |
| `process` | 5 | a sample in the queue, or 30 ms without one | runs lib/bms on the sample under the pack mutex, builds, protects and publishes the protected messages; judges the sample (or the timeout) for the fault monitor and posts the verdict to `health` |
| `can_tx` | 4 | a producer or the controller (task notification), or the queue's next deadline, at the latest after 50 ms | loads the controller's three mailboxes from the transmit queue, handles transmit-done events and hands those of UDS_Response frames to the diagnostic task, the bus-off policy |
| `diag` | 3 | a UDS_Request frame or a report of a UDS_Response frame sent, a flash job done, a clear made (task notification), the next deadline of the connection, at the latest after 50 ms | the diagnostic connection ([app/diag.h](../app/diag.h)): lib/isotp and lib/uds on 0x7E0 and 0x7E8, at most 2 frames and reports in a tick; the UDS backend ([app/diag_backend.h](../app/diag_backend.h)): the flash jobs of a download and of the confirm routine, the boot information, the DTC snapshot and the clear; the ECUReset |
| `health` | 2 | every 100 ms (`xTaskDelayUntil`) | the supervision cycle of the watchdog manager and the kick of the hardware watchdog; the DTCs (the verdicts, the CAN controller, the supervision, clears, those of ClearDiagnosticInformation too) and BMS_Fault in every period, and the snapshot of the DTCs for the diagnostic task; whether a trial image runs in good health, and once it has for 5 s the request for its confirm; BMS_TaskStats every second period (one task per frame), BMS_NodeInfo and a log line every tenth, BMS_BootInfo in the periods 2, 12, 22, ...; ends a run of `--duration-s` |
| `flash` | 1 | a job of the diagnostic task or the health task's request (task notification), and at the latest after 100 ms | every write of the node to the flash: the jobs of a download (the erase, each block, the exit, the check: lib/boot's slot writer) and of the confirm routine ([app/flash_job.h](../app/flash_job.h)), the confirm of a trial by itself (`boot_confirm()`), and at start, when the bootloader started the node, a read of the metadata for the log and the statistics. Without a flash every job ends with BOOT_E_STATE. Each flash operation blocks it for the time the part takes (a page program 0.4 ms, a sector erase 45 ms; 3 ms and 400 ms at most, [plat_flash.h](../platform/include/plat_flash.h)) |

Every task's stack is 512 words (`APP_TASK_STACK_WORDS`, `TASK_STACK_WORDS` in can_io.c), the
size meant for the target; on the host each task runs on a pthread stack of its own, which the
platform measures ([README](../README.md#what-phase-0-contains)). In the final run of
bench/node_timing.py ([results/node_timing.json](../results/node_timing.json), `stack_max`) the
deepest use of its host thread's stack by a task's code was the health task's, 4648 bytes
against the 4096 bytes (512 words of 8 bytes) it was created with, under the SocketCAN download
and the UDP flood (2824 bytes in the six other conditions), and the idle task used 2072 bytes
against its 2048: host depths with x86-64 frames and the port's signal frames, not an overflow
of the target, whose high-water marks phase 7 measures.

**Why the flash task is the lowest.** A flash operation is synchronous and slow: the driver of a
serial NOR polls the part's busy flag, and the host's flash blocks the calling task one tick at a
time until the modelled time has passed. A task with a deadline would spend that time waiting: a
sector erase is 45 ms, almost half a health period (400 ms at the part's maximum), and an erase of
a whole slot is 256 of them. Below every other task the waits delay nobody; the only cost to the
others is its short critical sections (see the analysis below).

**How the flash task is supervised (phase 5).** In phase 4 it had one job per run at most and was
no supervised entity; a download gives it hundreds, and a job that never returns (a part whose
busy flag never clears) must not leave the tester waiting for ever. It is now an entity of the
watchdog manager: it waits for a job at most 100 ms and gives an alive indication after every wait
and after every call of lib/boot in a job. Its window is 6 supervision cycles (600 ms, no less than
555 ms with the health task's response time), longer than the longest call of a working flash task
at the part's maximum times (a record: a sector erase and three page programs, 409 ms) with its own
response time (under 70 ms) on top, so erase steps of 400 ms back to back keep it OK; with a failed
tolerance of 1 a flash task that stops is EXPIRED 1.2 to 1.8 s after its last indication, and the
hardware watchdog resets the node 500 ms after the last kick. Meanwhile the diagnostic task, which
does not wait for the flash, answers responsePending every 2 s, so a download stuck in the flash
task ends in a watchdog reset, after which the bootloader starts the confirmed image (the slot
written stays WRITING or EMPTY, which it never starts) and the tester's request runs out of
P2*client. [tests/system/test_download.py](../tests/system/test_download.py) makes the first
program of a run hang (`--flash-hang 1`, [posix_flash.h](../platform/posix/posix_flash.h)) and
checks exactly that.

**Why the diagnostic task sits between `can_tx` and `health`.** Below `can_tx`, so that nothing of
the connection adds to `can_tx`'s response time, the smallest margin of the set (below). Above
`health`, whose budget is 5.5 ms: a responsePending that lib/uds produces 40 ms after a request has
to go out before P2server, 50 ms, and with `health` above it the diagnostic task's response could
be 45 ms. Phase 5 therefore moved the four tasks above it up by one priority, and
configMAX_PRIORITIES from 8 to 9: the levels 0 (idle) to 7 for the tasks, and 8 above every one
of them for the health task's orderly stop of the host build (`stop_node()` in
[app/app.c](../app/app.c), which asserts at compile time that no task has it). The stop reads
the counters and ends the scheduler at that priority so that no task runs in between; with
can_rx at 7 and the stop at 7 as well, time slicing let the receive task take a batch of frames
at the next tick after its counter was read, and the receive path's accounting
(`controller.rx_frames` against the driver's counts) came out about 30 frames short in 6 of 60
runs of the 1 Mbit/s flood test of [test_node_errors.py](../tests/system/test_node_errors.py),
and in none of 60 with the level kept free.

**How no consecutive frame is lost in the node.** The node's flow control asks for blocks of 16
consecutive frames (BS 16, STmin 0, [app/diag.h](../app/diag.h)). A tester that keeps to ISO 15765-2
sends no more than one block before it waits for the next flow control, which the diagnostic task
sends only after it took the block's last frame out of its queue; and it sends its next request
only after the node's response. So the queue between `can_rx` and `diag` holds at most 16 of its
frames, whatever the diagnostic task's timing; it has 18 places (`APP_DIAG_RX_QUEUE_LENGTH`). The
controller's receive FIFO in front of `can_rx` is drained by the highest-priority task, in batches
that cover a full bus at 1 Mbit/s (phase 1's analysis, bug 007), also under a flood of other
frames. A frame the queue refuses is counted (`diag.task.rx_lost`), and a lost consecutive frame
ends its reception in lib/isotp (`rx_results.WRONG_SN`, `TIMEOUT_CR`). The node never keeps the
receive buffer (lib/uds copies the request in the indication), so it never sends FC WAIT.

**Rate-monotonic order, with one exception.** A task with a shorter period has the higher
priority: `can_rx` (modelled per tick, T = 1 ms; frames can come every 47 µs) above `sensor` and
`process` (10 ms) above `diag` (modelled per 10 ms) above `health` (100 ms). `sensor` and `process` share a period; `sensor` is
above, because `process` waits for its samples. The exception is `can_tx`, which is released by
every frame it loads and so more often than the tasks above it. It runs below the tasks that
produce the frames on purpose: a producer is never preempted by its own output, and the frames a
period produces go out together. The design notes said the exception would be `can_rx`;
modelled by its demand per tick (T = 1000 µs, see below), `can_rx` has a period no longer than
that of any task below it and is in rate-monotonic order; the task that is not is `can_tx`. The
response-time analysis below holds for any fixed priorities and is the verdict.

## Communication between the tasks

| From | To | Mechanism | Why this one |
|---|---|---|---|
| CAN interrupt | `can_rx`, `can_tx` | task notification, one bit per cause (`xTaskNotifyFromISR`, `eSetBits`) | each task waits for "something for you" from one interrupt, with a few causes that may coincide. Bits in the notification value carry them without a kernel object per cause, and a bit set while the task is busy is still there when it next waits |
| any producer | `can_tx` | the transmit queue ([lib/can/can_txq.h](../lib/can/can_txq.h)) in short critical sections, then a notification bit | a cyclic message needs a slot that keeps only the newest value, not a FIFO: a busy bus must delay data, not queue stale values. The queue is plain C shared by several tasks, so it is guarded by `taskENTER_CRITICAL` for the few instructions an operation takes, cheaper than a mutex for that length |
| `sensor` | `process` | FreeRTOS queue of 4 samples (`app_sample_t`: lib/bms's `bms_sample_t` and the groups the front end did not answer, copied), statically allocated | a copy decouples the two in time and in memory: the sensor task can take the next sample while the last one is processed. The sensor task never waits for room (timeout 0): a full queue costs a sample, which is counted (`sample_queue.full`) and which lib/bms sees as a gap in the sequence numbers, but it never holds up the time base. The processing task waits with a timeout of 30 ms, three sensor periods: a timeout means the sensor task has stopped delivering, and is counted (`process.timeouts`) and judged as the fault SAMPLE_TIMEOUT. The processing task makes no log call; the health task logs the start and the end of such a stall from its counts (`process.timeouts`, `process.recoveries`) |
| `process` | `health` | the pack state (`bms_pack_t`) behind a mutex with priority inheritance | shared state that a reader must see whole, written by one task and read by another. The mutex is held only to run lib/bms on one sample or to copy the state, and frames are built from the copy outside the lock. While `health` (2) holds it, `process` (5) waiting for it lifts `health` to 5, so no task between them (`can_tx`, `diag`) can prolong the wait |
| `can_rx` | `process` | the telemetry switch, one `bool` read and written in critical sections | one word, set from TESTER_Command and read once per sample: a critical section around one load or store is the cheapest correct way |
| `process` | `health` | a FreeRTOS queue of 32 verdicts on the samples (`fault_verdict_t`, 8 bytes: two masks and the count of clear requests the processing task read as it posted the verdict, [app/fault.h](../app/fault.h)), statically allocated; the processing task never waits for room, a lost verdict is counted (`process.verdicts_lost`) | the DTCs have one owner, the health task; the processing task judges each sample where the data is and hands over the verdict in its order, so that the debouncing counts samples, and the count tells the health task which verdicts came after a clear request. The owner sits below `can_tx`: the DTC work in the processing task would have added to `can_tx`'s response time, which the analysis below does not allow (see [the task table](#the-result-for-the-nodes-task-table)) |
| `health` | `process` | the summary of the DTCs (`fault_summary_t`), written after every health period and read once per sample in critical sections | PackFaultActive and PackState FAULT follow the faults active in the summary; a few words, like the telemetry switch |
| `can_rx` | `process`, `health` | the count of clear requests of TESTER_Command, written and read in critical sections | the processing task tags each verdict with it, and the health task carries out a clear once per new count (`command_rx_t.clear_requests`), before the first verdict that carries it |
| `sensor`, `process`, `can_rx`, `can_tx` | `health` | alive indications to the watchdog manager ([lib/wdgm](../lib/wdgm/wdgm.h)), one lock-free atomic addition each | a task that hangs stops counting; the health task examines the counts once per period and kicks the hardware watchdog only while the manager allows it |
| bootloader | every task | the hand-off (`plat_boot_handoff()`), read once by `app_start()` before the scheduler starts and constant after | written before the image runs, read-only while it runs: no lock |
| `health` | `flash` | a task notification bit, once per run when the trial has run 5 s in good health | one request and no data: the slot to confirm is the hand-off's, which both tasks read |
| `flash` | `health` | the result of the confirm (`boot_status_t` and a flag), and whether the tester's routine confirmed the trial, written and read in critical sections | a few words; the health task takes them in its next period and updates the confirm's state (BMS_BootInfo) |
| `can_rx` | `diag` | a FreeRTOS queue of 18 UDS_Request frames (`can_frame_t`), statically allocated, and a notification bit; `can_rx` never waits for room, a frame it refuses is counted (`diag.task.rx_lost`) | the frames have to be taken in their order, by a task below the one that receives them; why 18 is enough is above |
| `can_tx` | `diag` | a FreeRTOS queue of 8 reports of UDS_Response frames sent (the time each was queued with), and a notification bit (`can_io_on_tx_done()`) | lib/isotp needs every confirmation, in the order of the frames; the time identifies the frame, so that a report that comes after lib/isotp gave its frame up is never taken for the next frame ([app/diag.h](../app/diag.h), CONFIRMATIONS) |
| `diag` | `can_tx` | the transmit queue's event FIFO (`can_io_send_event_stamped()`), with a notification only when no other event frame is held | a response must not be replaced or reordered; a frame queued behind another event frame is loaded when that frame is reported, so a burst costs `can_tx` one pass (the analysis below) |
| `diag` | `flash` | a mailbox of one job (`flash_job_t`) and its result, in critical sections, and a notification bit each way | lib/uds runs one operation at a time, so one job is enough; the data of a TransferData block is lib/uds's copy, unchanged until the job is done |
| `health` | `diag` | the DTCs' snapshot (a copy of lib/dtc's state, about 150 bytes on the target), the count of clear requests carried out and the confirm's state, written after every period in a critical section; a notification bit after a clear | the DTC services read one consistent picture of the end of the health task's last period, at most one period and the health task's response time (45 ms) old; the clear is made by the DTCs' only owner |
| `diag` | `health` | ClearDiagnosticInformation as one more clear request, added to those of TESTER_Command, in a critical section | the health task carries out every new count as it did for TESTER_Command alone, in order with the evidence (run_faults()), and the positive response waits until the count carried out has reached the diagnostic task's |
| every task | statistics | counters each written by one task only; the health task copies them at the end after raising itself to the highest priority | no task is in the middle of an update when they are read |

Every kernel object of the application is allocated statically (`xTaskCreateStatic`,
`xQueueCreateStatic`, `xSemaphoreCreateMutexStatic`); `lib/` and `app/` never allocate memory.

## From a sample to a frame

1. **Release.** `sensor` returns from `xTaskDelayUntil` at its period's tick (pdFALSE, a period
   that started late, is counted in `sensor.overruns`). After such an overrun it does not make
   up the periods that have passed since: it skips them (`sensor.skipped_periods`) and runs the
   period the tick is in, on the grid of its period.
2. **Heartbeat.** It publishes BMS_Heartbeat first: the number of the period (from 1; a count of
   the periods it ran, so that a skipped period leaves no gap) and the tick count, the status
   frame of phase 1 bit for bit, from transmit-queue slot 0.
3. **Acquisition.** It reads the front end ([plat_sensor.h](../platform/include/plat_sensor.h)):
   the current in every period, the cells in every second (sample numbers 0, 2, 4, ...), the
   temperatures in every tenth (0, 10, 20, ...). The sample is stamped with `plat_time_ns()` and
   numbered. A group the front end did not answer goes in with every channel flagged invalid,
   so that it becomes "not available" downstream instead of keeping its last values.
4. **Queue.** `xQueueSend(..., 0)`: the sample is copied into the queue, or counted as lost.
5. **Processing.** `process` takes it, and under the pack mutex runs `bms_process()`
   ([lib/bms](../lib/bms/bms.h)): every reading is checked against what its sensor can deliver
   (0 to 5000 mV, −40.0 to 125.0 °C, ±300 A) and against the front end's own invalid flag; a
   valid one goes through a first-order fixed-point filter per channel (shifts 2 for the
   current, 2 for the cells and 1 for the temperatures, `s_bms_config` in app.c; a time
   constant of −1 / ln(1 − 2^−shift) samples, 3.5, 3.5 and 1.4, which at 10, 20 and 100 ms per
   sample is about 35, 70 and 144 ms); an invalid one makes its channel invalid
   and resets its filter. Then the pack summary: the pack voltage as the sum of the filtered
   cells (valid only while all eight are), the lowest and highest cell, and the pack state
   (STARTUP until every channel has reported, FAULT while one is invalid, NORMAL otherwise).
6. **Conversion.** Outside the lock, from the copy: [app/signals.c](../app/signals.c) turns mA,
   mV and tenths of a degree into the raw values of the DBC. Divisions round half away from
   zero, as the filters do. A valid value beyond its signal saturates and is never sent as
   "signal not available": a cell below 1.000 V goes out as 1.000 V, as the DBC's comment says.
   SNA is sent only for an invalid channel; PackCurrent and PackVoltage, which have no SNA code,
   go out as 0 with their valid flag 0. One `_Static_assert` per factor, offset, SNA code and
   range ties the arithmetic to the generated header.
7. **Frame.** [app/telemetry.c](../app/telemetry.c) fills the generated codec's struct, packs it
   ([app/gen/can_db.c](../app/gen/can_db.c), generated from the DBC) and protects it with
   `e2e_p11_protect()`, one counter per protected message. BMS_PackStatus goes out with every
   sample, the cell messages with every sample that carries cells, BMS_Temperatures with every
   sample that carries temperatures, all of them only while the telemetry switch is on.
8. **Transmission.** `can_io_publish()` puts the frame into its message's slot of the transmit
   queue, with the sample's time stamp, and notifies `can_tx`, which loads it into a mailbox when
   one is free; the controller model sends it when it wins arbitration
   ([can-simulation.md](can-simulation.md)).

**Who sends what.** Each message has exactly one sending task, which alone writes its counters.

| Message | Sender | When | Switched off by CmdTelemetryOn = 0 |
|---|---|---|---|
| BMS_Fault 0x010 | health | every health period, 100 ms, after the period's last change of the DTCs: they change only in that job, so every change goes out at once, and a fault that came and went within the period is active in its frame. A fault of the pack values reaches its DTC in the first health period after the sample that decided it (at most 100 ms) | no |
| BMS_PackStatus 0x020 | process | every sample, 10 ms | yes |
| BMS_Heartbeat 0x030 | sensor | first thing in every period, 10 ms | no |
| BMS_CellVoltages1/2 0x040/0x041 | process | every sample with cells, 20 ms | yes |
| BMS_Temperatures 0x050 | process | every sample with temperatures, 100 ms | yes |
| BMS_TaskStats 0x600 | health | every 200 ms (the odd health periods), round robin over can_rx, sensor, process, can_tx, health, flash, IDLE | no |
| BMS_NodeInfo 0x610 | health | every second | no |
| BMS_BootInfo 0x611 | health | every second, 200 ms after BMS_NodeInfo | no |
| BMS_Pong 0x6F1 | can_rx | for every ping | no |

The heartbeat comes from the sensor task because its two numbers describe that task: the
number of the period it runs and the tick it was released in. The sensor task is the highest-priority
periodic task and is released by the tick alone, so the numbers keep the timing of phase 1's
status frame. The frame reaches the bus only when `can_tx` loads it, and `can_tx` (4) runs below
`process` (5): the heartbeat goes out after the processing job of its period, and a heartbeat on
the bus says that the sensor task and `can_tx` both run. A processing task that blocks shows on
the bus by itself: its protected messages stop while the heartbeat goes on, and their receivers'
end-to-end checks time out. A processing job that does not end keeps `can_tx` from running, as
any task at priority 4 or above that keeps the CPU does: the heartbeat then stops with every
other frame of the node, which a receiver cannot tell from a time base that stopped. The
watchdogs of phase 3 find both: the watchdog manager a task that stops reporting that it is
alive, the hardware watchdog a health task that no longer kicks it (SUPERVISION in
[app/app.c](../app/app.c)).

**The way back: TESTER_Command.** `can_rx` owns the command receiver
([app/command.c](../app/command.c)): it checks every TESTER_Command frame on arrival (AUTOSAR E2E
Profile 11 and the E2E state machine) and, at the end of every 100 ms receive cycle without a
frame, checks with "no new data". A command is carried out only while the state machine is VALID
and only for a new, intact frame; then CmdTelemetryOn becomes the telemetry switch, and a
CmdClearFaultsSeq that differs from that of the command carried out before it, in a stream that
stayed VALID between the two, is a request to clear the DTCs, which the health task carries out
in its next period, in order with the evidence ([faults.md](faults.md#the-fault-frame-bms_fault-0x010)). The parameters and what
follows from them are in
[can-protocol.md](can-protocol.md#the-nodes-receiver-of-tester_command). After a host stall the
receive cycle makes one check for all the periods it missed and counts the others
(`can.driver.rx_cycles_skipped`), so that the command stream does not go INVALID because the
node, rather than the tester, stood still.

## Statistics

`--stats-json` writes, besides what phase 1 wrote (unchanged):

- `task_table`: the task table of the schedulability check (below), schema `fcn.task_table/1`;
- `sensor`, `sample_queue`, `process` and `pack`: periods, reads and unanswered reads per group,
  samples posted and lost, samples processed, timeouts, backlog, and the final pack state with
  its counters (flagged and implausible readings, lost samples, sequence errors);
- `telemetry.messages`: per cyclic message what the application did with it (published,
  suppressed by the switch, failed) and its transmit-queue slot's counters, produced = sent +
  superseded + pending;
- `command`: the Profile 11 status counts, the checks, the state machine's state and counts, the
  commands carried out and ignored, the switch and the last CmdClearFaultsSeq;
- `overruns` per task, and `can.driver.rx_cycles` and `rx_cycles_skipped`;
- `reset`: the reset cause of this start from the retained-RAM cell (`--retained-file`,
  [plat_retained.h](../platform/include/plat_retained.h)): `cause` and `cause_code` (the value of
  `plat_reset_reason_t`), `retained_cell`, the `verdict` on the cell and `resets_since_power_on`;
- `hw_watchdog`: what the hardware watchdog model counted
  ([plat_wdg.h](../platform/include/plat_wdg.h)): `started`, `timeout_ms`, `kicks`,
  `longest_unkicked_ns`, and the host stalls it did not count (`gaps`, `longest_gap_ns`);
- `faults`: the DTCs as their owner, the health task, left them ([app/fault.h](../app/fault.h)):
  the summary the last BMS_Fault carried (`pack_fault_active`, `active`, `confirmed`,
  `most_recent`, `warning`), `clears`, `clear_requests_seen`, `verdicts_applied`,
  `health_cycles`, `status_changes`, the `reset_cause` the reset faults were judged from, and per
  fault in `dtcs` its `event` number, `name`, `dtc` number and its display `code` (`P1A01-00`), the
  `status` byte (ISO 14229-1), the debounce `counter`, the `fdc` and `failed_cycles`;
- `watchdog_manager`: the supervision ([lib/wdgm](../lib/wdgm/wdgm.h)): `global_status`,
  `cycles`, `first_expired`, `expired_cycles`, `expired_tolerance`, the health task's `kicks` and
  `kicks_withheld`, `hw_started`, `hw_timeout_ms`, and per entity its `status`, `examinations`,
  `incorrect`, `last_count`, the fewest and most alive indications in one cycle (`min_count`,
  `max_count`) and its configuration;
- in `process`, `verdicts_posted`, `verdicts_lost` and `recoveries` (samples that came after a
  timeout); in `sensor`, `skipped_periods` (the periods skipped after an overrun, which `periods`
  does not count); in `command`, `clear_requests`; in `can.driver`, `error_passive_entries`;
- `flash` (phase 4): what the host's flash counted ([posix_flash.h](../platform/posix/posix_flash.h)):
  `file`, `created`, the reads, programs, units programmed, erases and refused calls, `busy_ns`,
  the erases per area (`erases_meta`, `erases_slot_a`, `erases_slot_b`), `max_sector_erases`, and
  the cut (`cut`: none, armed or fired, `cut_in`, `cut_seed`);
- `handoff` (phase 4): the bootloader's hand-off as the node took it
  ([plat_retained.h](../platform/include/plat_retained.h)): `verdict` (taken, invalid, none),
  `present`, `slot`, `mode` (confirmed or trial), `attempt`, `image_version` and its
  `image_version_code`, `event` (NONE, ADOPT, ROLLBACK, REJECT, FALLBACK), `event_slot`,
  `select_write_error`, and `rolled_back` (the evidence of UPDATE_ROLLBACK);
- `diag` (phase 5): the diagnostic connection: `session`; `task` (the diagnostic task:
  `events`, `passes`, `budget_waits`, `events_per_tick`, `rx_queue_length`, `rx_lost`,
  `rx_high_water`, `tx_done_queue_length`, `tx_done_lost`); `connection` ([app/diag.h](../app/diag.h):
  `frames_in`, `frames_out`, `frames_refused`, `confirms`, `confirms_passed`, `confirms_late`,
  `confirms_unknown`, `confirms_forgotten`, `requests`, `responses`, `responses_sent`,
  `responses_failed`, `responses_dropped`, `resets`); `isotp` (its `parameters`, and lib/isotp's
  counters: `tx_requests`, `tx_results` and `rx_results` by N_Result, `ff_indications`,
  `frames_out` and `frames_in` by kind, `ignored` by reason, `rx_overflow`, `rx_sf_too_long`,
  `rx_sf_busy`, `fc_dropped`, `stmin_reserved`, `send_refused`, `confirmed`, `confirm_unmatched`,
  `unconfirmed`, `proven`, `outstanding`, `reentry_refused`, `n_br_late`, `n_cs_late`,
  `n_br_max_us`, `n_cs_max_us`); `uds` (lib/uds's: `requests` by service, `requests_unsupported`,
  `requests_empty`, `nrc` by name, `response_pending`, `positive`, `suppressed`, `session_changes`,
  `s3_timeouts`, `resets`, `pending_max_us`, and `download`: `state`, `address`, `size`,
  `received`, `blocks`, `repeated_blocks`, `counter`, `erases`, `started`, `exited`, `committed`,
  `aborted`, `failed`, `erase_us`, `transfer_us`, `check_us`); `backend` ([app/diag_backend.h](../app/diag_backend.h):
  `post_refused`, `snapshots`, `clears`, `clears_done`, `resets`, and per job kind `posted`,
  `failed`, `last_steps`, `last_status`); in `can.driver`, `tx_done_reports` and
  `tx_quiet_enqueues`; in `confirm.flash_task`, `confirmed_by_routine`, `self_skipped`,
  `idle_passes`, and per job kind (`jobs`) `run`, `last_status`, `last_steps`, `last_ns`,
  `max_ns` and (phase 6) `total_ns`, their sum. A run that ends in a reset writes no
  statistics: a download run that the tester ends with ECUReset has its observables in its log
  and in the tester's report;
- `confirm` (phase 4): the confirm of a trial ([app/update.h](../app/update.h)): `state`
  (NOT_NEEDED, WAITING, CONFIRMING, CONFIRMED, FAILED), `confirm_after_s`, `periods_needed`,
  `periods` judged, `healthy_periods`, `window_restarts`, `requested_period`, the periods that
  failed each condition of good health (`unsupervised_periods`, `not_active_periods`,
  `bus_off_periods`, `unacked_periods`), the frames acknowledged in the window so far
  (`window_acked`) and the fewest in one of its periods (`fewest_acked`), `status` (lib/boot's
  result), and the flash task's own counts in `flash_task`: `flash`, `boot_init`, `described`
  and what the metadata said at start (`have_record`, `seq`, `active_slot`, `trial_slot`,
  `attempts_used`, `records_valid`), `confirms`, `confirm_status` and `confirm_ns`, how long the
  confirm took with its waits for the flash;
- `build_id` (phase 6): the build id of the tree the node was built from
  ([platform/posix/build_id.h](../platform/posix/build_id.h)): the commit, and `-dirty-` with a
  hash of the uncommitted changes for a tree that had them, as `--version` and a line of the log
  at the start say it too. A bench (bench/README.md) refuses to write a result whose node, read
  here or from `--version`, was not built from the tree its provenance names. It is host-only:
  the application never reads it.
- `trace` (phase 6): `false`, or for a node started with `--trace-file` the host-only trace of
  the scheduler and the simulated hardware that bench/node_timing.py reads
  ([platform/posix/posix_trace.h](../platform/posix/posix_trace.h)): its `capacity`, the
  `events` recorded and `dropped` (beyond the capacity, `--trace-events`), `record_bytes`,
  `file_version`, `event_cost_ns` (what one record cost when the trace was opened: the fastest
  of the rounds timed, `cost_records` records in all), and the `number` and `name` of every task
  it saw run. The trace records every task switch (with whether the task switched out had
  blocked), every move of a task to the ready list, every block in `xTaskDelayUntil()` with its
  wake tick (the kernel's trace macros, defined in config/posix/FreeRTOSConfig.h), every frame of
  the node the CAN model completed with its end of frame and the time of its write, every sample
  the sensor task read, and when every tick was due (posix_tick.c's base), all on
  CLOCK_MONOTONIC. It is host-only, records nothing without the option, and changes nothing the
  application does: the hooks read the kernel's state and write a buffer allocated and written
  once before the scheduler starts.

## Schedulability

[tools/fcn_tools/sched.py](../tools/fcn_tools/sched.py) checks whether the node's tasks meet their
deadlines under fixed-priority preemptive scheduling, the way FreeRTOS schedules them:

```bash
python -m fcn_tools.sched <table.json> [--json report.json]
```

It prints a report. It exits with 0 when every task meets its deadline, 1 when one does not, and
2 when it refuses the input. `--json` also writes the report as JSON (schema
`fcn.sched_report/1`).

### The task table

The input (schema `fcn.task_table/1`) has one entry per task:

| Field | Meaning |
|---|---|
| `name` | unique |
| `priority` | the FreeRTOS priority: a higher number is more urgent |
| `kind` | `periodic`: a job arrives every `period_us`, the period of the task's `xTaskDelayUntil` loop. `sporadic`: jobs arrive at least `min_interarrival_us` apart |
| `deadline_us` | at most the period; counted from the arrival |
| `wcet_us` | the execution-time budget of one job, including its critical sections, kernel calls and context switches |
| `jitter_us` | release jitter: how long after its arrival a job can still be waiting to become ready |
| `nonpreemptive_us` | the longest stretch of a job that no other task can preempt (`taskENTER_CRITICAL`, `vTaskSuspendAll`); 0 if there is none |
| `critical_sections` | for each mutex the task takes, the longest time it holds it in one job |

All times are integers in microseconds, and every field is required. Unknown fields, numbers
that are not integers and duplicate keys are refused, so a misspelt field cannot fall back to a
default. The optional `wcet_basis` says whether the execution times are `design` budgets or
`measured`.

The node writes the table it runs with into its statistics (`task_table`). It comes from
[app/task_table.c](../app/task_table.c), whose priorities and periods are the constants the tasks
are created with, so it cannot drift from the code that creates them.
`tests/system/test_telemetry.py` runs `python -m fcn_tools.sched` on the table of a running
node and fails on any exit status other than 0; it also checks the table's priorities against
the tasks that ran and its periods against the cycle times of the DBC.

The execution-time budgets and critical-section lengths in the table are design figures for a
Cortex-M3 class target; each is derived in a comment in app/task_table.c. The table says so
(`"wcet_basis": "design"`), and the report repeats it on its second line. Phase 6 does not
replace them with measurements: it runs on the host and has no target. What it measures instead
is the host's run time per task, from FreeRTOS's run-time statistics (`run_time_us`), and per
job from the node's host-only trace ([bench/node_timing.py](../bench/node_timing.py),
[docs/measurements.md](measurements.md)): x86-64 times of a host thread, which say nothing about
a Cortex-M3. The final run's figures per task, transport and condition are in
[results/node_timing.json](../results/node_timing.json) (`cpu.tasks`), host figures next to the
design budgets, not in their place. Phase 7 builds the node for a Cortex-M3 under QEMU, which
is not cycle-accurate either. "Schedulable" therefore means: schedulable as long as no job runs
longer than its
budget.

### How the node's tasks are modelled

The rules (Audsley, Burns, Richardson, Tindell and Wellings 1993, and those of the model below),
as app/task_table.c applies them:

- **Interrupts** are sporadic tasks above every task, with a priority of `configMAX_PRIORITIES`
  (9) or more: `can_irq` (10) at the shortest frame on the bus at the highest bit rate the node
  accepts (44 bits and the 3-bit intermission at 1 Mbit/s: 47 µs), so that every bit rate is
  covered, and the kernel's tick, `tick_irq` (9), periodic with T = 1000 µs
  (`configTICK_RATE_HZ` is 1000).
- **`can_rx` and `can_tx`** are released by events that can come far more often than once per
  tick, so each is modelled by its demand per tick: one job per tick (T = 1000 µs) that does what
  any 1000 µs can ask of the task. Every wake-up of either is one pass of its loop in
  [app/can_io.c](../app/can_io.c), so the budget counts passes as well as frames. At 1 Mbit/s at
  most 22 frames end on the bus in any 1000 µs, one per 47 µs.
  - `can_rx`: at most one pass per received frame and one for the receive cycle, 23 passes of
    3 µs; 22 frames of 4 µs; the receive cycle's 12 µs. 169 µs.
  - `can_tx`: at most one pass per frame that ends on the bus (the transmit-done of one of the
    node's frames, or the pong that a ping makes `can_rx` queue), one for `sensor` and `process`
    together (it runs only once both have blocked), two for `health` (which is below it, so each
    publish wakes it at once, and BMS_TaskStats and BMS_NodeInfo can fall into one period) and
    one timeout of its wait: 26 passes of 13 µs. A load and a transmit-done for each of the
    node's frames that ends, and a load for each of its 3 mailboxes: 47 events of 3 µs. 479 µs.

  A busy period of a lower task begins with no work of theirs left over, so all they do in it
  is caused within it, at most one budget per 1000 µs: the interference the analysis charges is
  an upper bound. The model is that of a bus without faults, as for `can_irq`: error frames, the
  controller's state changes and the transmit queue's aborts of frames in the mailboxes are
  outside it. The derivation is in app/task_table.c. Modelled per event instead (T = 47 µs),
  `can_tx` would be unschedulable, because it runs below `sensor` and `process` by design.
- **A task released by another task** is periodic with the releasing task's period, and its
  release jitter is the releasing task's worst-case response time (Audsley et al. 1993, section
  4). `process` is released by the sensor task's sample: T = 10 ms and J = 2000 µs, the sensor
  task's deadline, which bounds its response time. Its deadline and response time count from the
  sensor task's arrival, so its response time is the latency from the tick.
- **Blocking.** Every task that calls can_io enters critical sections around the transmit
  queue, at most 5 µs; the health task suspends the scheduler while `uxTaskGetSystemState()`
  reads the task list (20 µs); `process` and `health` share the pack mutex (40 and 10 µs).
- **Phase 3** added to two budgets. `process`: the verdict on each sample for the fault monitor
  and its post to the health task, 10 µs (150 → 160 µs). The processing job makes no log call:
  a log line costs 100 µs, more than `can_tx`'s margin below, so the lines about a stall of the
  samples are the health task's, written from the processing task's counts. `health`: the
  supervision cycle and the kick (17 µs), up to a queue full of verdicts worked into the DTCs
  (32 × 21 µs: each taken out and its tag of clear requests compared, then up to nine reports
  with the read of testFailed after each for the summary of the period; the task takes no more
  than that in one period), up to a clear before each verdict and one after them (33 × 15 µs),
  the CAN controller's counters and monitors and the summary (40 µs), BMS_Fault (25 µs), and
  log lines of 100 µs: two for every fault, which can rise and fall within one period, one for
  the clears, one for the expiry of a supervised task and two for a stall of the samples
  (36 × 100 µs): 300 → 5149 µs, most of it the log lines of a period in which every fault
  changes twice, which does not happen; the health task is the lowest-priority task, so its
  budget delays no other. The health task's
  frames are spread over its periods (BMS_TaskStats in the odd ones, BMS_NodeInfo in every tenth),
  so a period publishes at most two and `can_tx`'s count of passes for it stays at two. The DTC
  work belongs to the health task and not to the processing task, the task that sees the
  samples, because the processing task is above `can_tx`: each microsecond in its budget is one
  more in `can_tx`'s response time. With the owner's work in the processing task (its budget
  160 + 54 = 214 µs: the reports, the summary and BMS_Fault) the analysis finds `can_tx`
  unschedulable, R = 1209 µs against its deadline of 1000 µs.
- **Phase 4** added the flash task and five microseconds to the health task's budget.
  `health`: the period judged for the confirm of a trial and, once per run, the request to the
  flash task (5 µs); one more fault, UPDATE_ROLLBACK, adds two possible log lines (200 µs):
  5149 → 5354 µs. BMS_BootInfo goes out in a period without BMS_TaskStats and BMS_NodeInfo, so
  a period still publishes at most two frames and `can_tx`'s count of passes for the health task
  stays at two. `flash`: sporadic, released at most once per health period (T = D = 100 ms), its
  budget the CPU time of one confirm: two scans of the 64 entries of the metadata log (30 µs an
  entry), the record handed to the driver (100 µs) and two log lines (100 µs each): 4140 µs. Its
  waits for the part block it and take no processor time, so they are not in the budget; a job
  that waits in the middle is outside the model (see [Filling in the table for another
  task](#filling-in-the-table-for-another-task)), which for the lowest-priority task lengthens
  only its own response. Its critical sections (a log line's push, the result handed back) are
  counted as 5 µs, those of the transmit queue.
- **Phase 5** added the diagnostic task and changed four entries.
  - `diag` (priority 3): released by every UDS_Request frame, every report of a UDS_Response
    frame, a flash job done, a clear made and its own deadline, so like `can_rx` modelled by its
    demand per window, here of 10 ms, with a deadline of 10 ms (P2server's 50 ms less the 40 ms
    of the first responsePending). It takes at most 2 frames and reports in a tick
    (`APP_DIAG_EVENTS_PER_TICK`), so a window has at most 20: 6 µs each through lib/isotp and
    into the transmit queue, 20 µs for the message each can complete (lib/uds's checks, the copy
    of up to 1026 bytes, a response of up to 131 bytes, a flash job posted), 20 µs for each of the
    22 responses the polls can produce (one per job, one job per request, and one
    responsePending), and 62 passes of 5 µs: 1270 µs. Without the cap a tester that floods 0x7E0
    at 1 Mbit/s could ask for 91 frames in 10 ms and make the set unschedulable; the cap is what
    bounds the task's demand, and with it the throughput of a download (79 ticks at least for a
    TransferData of 1024 bytes: 147 frames in and 11 reports; the final run of
    bench/isotp_throughput.py took 82.8 ms per block over SocketCAN and 82.9 ms over UDP in the
    median, host times, [results/isotp_throughput.json](../results/isotp_throughput.json)).
  - `can_tx`: one pass more, 26 → 27 (479 → 492 µs). A frame of the diagnostic task wakes
    `can_tx` only when no other event frame is held (`can_io_send_event_stamped()`); otherwise the
    report of the frame ahead of it, a pass counted already, loads it. So between two such
    wake-ups a diagnostic frame ended on the bus, and in any window there is at most one wake-up
    more than diagnostic frames that ended in it. Such a frame has 8 bytes (111 µs at 1 Mbit/s)
    and costs two passes with its report, 2 events and the report handed on, 36 µs per 111 µs of
    bus; a shortest frame costs 19 µs per 47 µs, more per µs of bus, so the worst window is still
    one of 22 shortest frames, and the diagnostic task adds the one pass.
  - `health`: the snapshot of the DTCs, the clears carried out and the confirm's state published
    in critical sections (5 µs), the notification of the diagnostic task after a clear (2 µs), and
    once per run a confirm of the routine taken into the monitor with its log line (105 µs):
    5354 → 5466 µs.
  - `flash`: its job is now a run of lib/boot's calls (an erase of any number of sectors, a
    block written, the validation of an image a step per 64 bytes), each of which waits for the
    part; modelled by its longest call, a record write (the confirm's budget of phase 4, now with
    the result handed back and the notification of the task that waits for it: 4145 µs). Its jobs
    take whatever processor time the other tasks leave.
  - The priorities of `can_rx`, `sensor`, `process` and `can_tx` went up by one (7, 6, 5, 4),
    which changes no response time: the order is the same.
- **The idle task** is not listed. There are no software timers (`configUSE_TIMERS` is 0), so
  there is no timer task either.

### What each test proves

| Test | What it proves | What it assumes beyond the model below |
|---|---|---|
| U = Σ C/T ≤ 1 | Nothing when it holds. When it fails, no scheduler can meet every deadline with these budgets. | none |
| Liu and Layland (1973): U ≤ n(2^(1/n) − 1) | Schedulable when it holds. Nothing when it fails: it is a sufficient test only. | distinct rate-monotonic priorities; deadlines equal to periods; no blocking; no release jitter |
| Hyperbolic bound, Bini, Buttazzo and Buttazzo (2003): Π(C/T + 1) ≤ 2 | The same as Liu and Layland, and it accepts every set they accept, and more. | the same |
| Response-time analysis: Joseph and Pandya (1986); Audsley, Burns, Richardson, Tindell and Wellings (1993) | The worst-case response time of every task, compared with its deadline. This is the verdict of the report and the exit status. | any fixed priorities. Exact without blocking; an upper bound with it. |

When the table breaks an assumption of the two bounds, the report says "not applicable", names
the assumption, and does not evaluate the bound. Applied outside its assumptions, a bound can
accept a set that misses a deadline.

The response-time analysis follows each task's busy period over all of its jobs (Lehoczky 1990;
Tindell, Burns and Wellings 1994). For a task that misses its deadline it therefore reports the
worst response time, not only the first job's. A task that meets its deadline has a single job
in its busy period, and its response time is the classic one for the first job.

Blocking is that of priority inheritance (Sha, Rajkumar and Lehoczky 1990):

- A lower-priority task delays a task while it holds a mutex that this task, or a task above it,
  takes.
- A non-preemptive stretch of a lower-priority task delays every task above it.
- A job is blocked at most once on each such resource and at most once by each lower-priority
  task. The analysis takes the smaller of the two sums this gives.

Tasks of equal priority, which FreeRTOS time-slices, count each other as interference. All
arithmetic is exact, in integers and fractions.

The report also says whether the priorities are in rate-monotonic order and in
deadline-monotonic order. If they are not:

- The two bounds do not apply.
- The response-time analysis is unaffected: it holds for any fixed priorities, and its verdict
  stands.
- If the order is not deadline-monotonic, the report repeats the analysis with
  deadline-monotonic priorities. For deadlines no longer than periods, without blocking and
  jitter, that order is optimal (Leung and Whitehead 1982): if any fixed-priority order makes
  such a set schedulable, deadline-monotonic does. With blocking or jitter another order can
  still do better.

### The result for the node's task table

The table the node wrote in a run of one second, checked as the system test checks it:

```bash
"${FCN_BUILD_ROOT:-build}/release/bin/fcn_node" --duration-s 1 --stats-json stats.json
python -c 'import json; print(json.dumps(json.load(open("stats.json"))["task_table"]))' > task_table.json
python -m fcn_tools.sched task_table.json
```

```
Utilisation U = sum of C/T = 0.9557
  necessary          U <= 1                                       holds
  Liu and Layland    U <= n(2^(1/n) - 1) = 0.7205 for n = 9       not applicable: deadline shorter than period: sensor; release jitter: process; blocking by lower-priority tasks: can_irq, tick_irq, can_rx, sensor, process, can_tx, diag, health; priorities not in rate-monotonic order
  hyperbolic         prod(C/T + 1) = 2.3167 <= 2                  not applicable: deadline shorter than period: sensor; release jitter: process; blocking by lower-priority tasks: can_irq, tick_irq, can_rx, sensor, process, can_tx, diag, health; priorities not in rate-monotonic order

Priority order (a higher number is more urgent)
  rate-monotonic      no: can_tx (T 1000, priority 4) is not above sensor (T 10000, priority 6) (and 1 more pair(s))
  deadline-monotonic  no: can_tx (D 1000, priority 4) is not above sensor (D 2000, priority 6) (and 1 more pair(s))

  task      prio  kind           T       D     C     J   B      R  slack  job  verdict
  can_irq     10  sporadic      47      47     2     0  20     22     25  1/1  meets
  tick_irq     9  periodic    1000    1000     3     0  20     25    975  1/1  meets
  can_rx       7  sporadic    1000    1000   169     0  20    202    798  1/1  meets
  sensor       6  periodic   10000    2000   100     0  20    306   1694  1/1  meets
  process      5  periodic   10000   10000   160  2000  30   2484   7516  1/1  meets
  can_tx       4  sporadic    1000    1000   492     0  30    998      2  1/1  meets
  diag         3  sporadic   10000   10000  1270     0  25   5787   4213  1/1  meets
  health       2  periodic  100000  100000  5466     0   5  44913  55087  1/1  meets
  flash        1  sporadic  100000  100000  4145     0   0  69939  30061  1/1  meets

Verdict: schedulable: every task meets its deadline
```

(An excerpt; the report also lists the blocking of every task and the analysis with
deadline-monotonic priorities, under which the set is schedulable too.) Both bounds are not
applicable, as expected for this table: the sensor task's deadline is shorter than its period,
the processing task has release jitter, the critical sections and the pack mutex mean blocking,
and `can_tx` breaks rate-monotonic order. The response-time analysis is the verdict: with the
design budgets, every task meets its deadline. The smallest margin relative to its deadline is
`can_tx`'s, 2 of 1000 µs (37 in phase 2, 27 in phase 3, 22 in phase 4: the 10 µs added to the
processing task's budget in phase 3 are 10 µs more of `can_tx`'s response, in phase 4 the flash
task's critical sections add 5 µs to its blocking, and in phase 5 the diagnostic task's pass adds
13 µs to its budget and the pack mutex held by `health` decides its blocking, 30 µs). That margin
is as thin as it can be: any more work above `diag` would make `can_tx` miss its deadline in the
analysis, which is why the diagnostic task is below it, and why its frames wake `can_tx` only when
nothing else will. Most of `can_tx`'s 492 µs is what its passes cost whatever they carry: 351 µs,
13 µs for each of 27. The analysis adds up
the non-preemptive stretches of the lower-priority tasks (`can_tx`: the health task's 20 µs and
the flash task's 5 µs, B = 25), the smaller of its two sums; on one processor a job can be
blocked by only one of them, the one running when it arrives, so 20 µs would be the bound here
and the 5 µs are a pessimism of the analysis. The processing task's response, 2484 µs from the
tick, is mostly the 2000 µs of release jitter that the sensor task's deadline allows it. The
health task's response, 44913 µs, is under half of its period (19907 µs before phase 5), and the
flash task's, 69939 µs from its release, under three quarters of its: CPU time only, before its
waits for the part. The diagnostic task's, 5787 µs, is the bound of P2server's budget it needs
(10 ms). The health task's longer response moves the examinations of the watchdog manager by up to
45 ms; the bounds of the entities (SUPERVISION in [app/app.c](../app/app.c), [faults.md](faults.md))
were checked against it, and the processing task's upper bound went from 20 to 21. The largest blocking
term, 20 µs, is the health task's scheduler suspension, which the analysis counts against the
interrupts as well, a pessimistic choice for `vTaskSuspendAll()`, which does not mask
interrupts.

### The model, and where the POSIX port leaves it

Every result of the analysis is about this model of the node:

1. One processor. The highest-priority ready job runs, and it preempts the running one at once
   (`configUSE_PREEMPTION`).
2. The jobs of task i arrive at least T_i apart. Each becomes ready at most J_i after it arrives.
3. A job runs for at most C_i. Before it completes, it waits for nothing but a mutex.
4. Mutexes have priority inheritance, as FreeRTOS mutexes do. Critical sections on them are not
   nested.
5. Interrupt handlers exist only as tasks of the table: sporadic tasks above every other
   priority, with the shortest time between two of their interrupts.

On a microcontroller, assumption 1 holds by construction: the core executes the task the kernel
chose until an interrupt arrives. On the FreeRTOS POSIX port it does not hold. Every task is a
thread of a Linux process. The FreeRTOS scheduler decides which thread may run, but the host's
scheduler decides when that thread gets a CPU (and under WSL2, Windows schedules the virtual
machine). A job can stand still in the middle of its execution while FreeRTOS considers it
running, and nothing in the model accounts for that time.

Assumption 3 is not a fact about the host either. An execution time measured on the host is
that of a laptop CPU under a general-purpose operating system, and it says nothing about a
Cortex-M. The tick and the simulated CAN interrupt are signals that host threads outside the
scheduler send to the thread of the running task ([posix_tick.c](../platform/posix/posix_tick.c),
[posix_irq.c](../platform/posix/posix_irq.c)). A periodic task is released as late as the host
delivers its tick. The final run of bench/node_timing.py also released three jobs early over
UDP, none by more than a tick: one sensor job 763 µs before its due time under the flood, and
one sensor job (776 µs) and one health job (918 µs) during the downloads
([results/node_timing.json](../results/node_timing.json), `release.<task>.released_before_due`).

On the POSIX port the check is therefore a check of the design: the task set, its priorities,
its budgets and its blocking. It does not predict the response times the node shows on the host.
A response time measured on the host that exceeds the analysed one does not contradict the
analysis. The final run of bench/node_timing.py has such cases in all six conditions it timed
([results/node_timing.json](../results/node_timing.json), `release.<task>.response` of each
transport and condition): the sensor task's longest response, analysed at 306 µs, was 628 µs on
an idle bus over SocketCAN and went past its deadline of 2000 µs in the SocketCAN download
(2219 µs) and over UDP under the flood (3556 µs), where a sensor job was released 3485 µs after
its arrival and a processing response took 3773 µs against the analysed 2484 µs (the p99.9 of
the sensor's release lateness and of that response there 900 and 987 µs, `summary.udp.flood`).
What carries over is what is counted rather than timed: which task can preempt or block which.

### Filling in the table for another task

- **A task released by several kinds of event** (a queue, notification bits, a timeout) has as
  many arrivals as those events can produce: give the shortest time between two of them.
  Notifications that coalesce let one job serve several events; the analysis still counts one
  job per event, so it stays an upper bound.
- **A job that waits mid-execution** for anything but a mutex (a queue, a delay, a notification)
  is outside the model. For the analysis it is two tasks, one per part.
- **An interrupt handler**: its execution time is the handler's, and its minimum inter-arrival
  time is the shortest time between two of its interrupts; for the CAN controller that is the
  shortest frame on the bus at its bit rate, which [fcn_tools.canbits](../tools/fcn_tools/canbits.py)
  gives.

### How the check is tested

[tests/python/test_sched.py](../tests/python/test_sched.py) checks the analysis against answers
that do not come from it:

- task sets whose response times are worked out by hand in its comments;
- the examples of Audsley et al. (1993), against the paper's text. The values the paper gives
  for Tables 1 and 2 come out exactly, and so do 16 of the 18 rows of Table 3. Row 11 matches
  only with a blocking time 1000 µs above the printed 1050 µs. The paper's row 12 is a larger
  solution of the same equation: the paper starts each task's iteration at the response time of
  the task above it, which assumes the lower task's response time is the longer one. Blocking
  can break that assumption, and here it does. Both rows are worked out in the test;
- a discrete-time simulation of the schedule as an independent oracle: preemption, priority
  inheritance and non-preemptive stretches, from the critical instant over the hyperperiod, on
  random task sets with integer parameters. The analysis is never below the simulated worst
  case, with blocking and with any release pattern within the jitter. It equals the simulation
  without blocking, both for synchronous releases and for the critical-instant pattern with
  jitter.
