# Diagnostics over CAN: ISO-TP and UDS

A tester on the bus talks to the node with UDS (ISO 14229-1:2020) over ISO-TP (ISO 15765-2:2016)
on two identifiers, and makes the update that `fcn_flash stage` makes ([update.md](update.md)),
with the node running: it erases the slot that does not run, downloads the new image into it,
has it checked and committed, resets the node, finds the trial started by the bootloader, and
confirms it. The same connection reads the boot information and the DTCs and clears them.

```
  tester: udsoncan 1.26.1 over can-isotp 2.0.7 (tools/fcn_tools/diag.py, flasher.py)
     │ UDS_Request 0x7E0                                    ^ UDS_Response 0x7E8
     v                                                      │
  can_rx (7) ──(a queue of 18 frames)──> diag (3): lib/isotp ──> lib/uds ──(backend)──┐
  can_tx (4) <──(event frames; their transmit-done reports, a queue of 8)── diag       │
  health (2) ──(the DTCs' snapshot, clears made)──> diag      flash (1) <──(jobs)──────┘
                                                              lib/boot's slot writer
```

| Part | Where | What |
|---|---|---|
| The transport | [lib/isotp](../lib/isotp/isotp.h) | ISO 15765-2 for one connection: pure C, time passed in, frames out through a callback |
| The server | [lib/uds](../lib/uds/uds.h) | ISO 14229-1: sessions and their timing, the general response behaviour, the services, the download's state machine; long work through a backend that answers *pending* |
| The node's connection | [app/diag.c](../app/diag.c), THE DIAGNOSTIC TASK in [app/app.c](../app/app.c) | lib/isotp in front of lib/uds, joined to the CAN tasks; the node's parameters |
| The node's backend | [app/diag_backend.c](../app/diag_backend.c), [app/flash_job.c](../app/flash_job.c) | the flash work as jobs of the flash task, the boot information, the DTC snapshot and the clear, the reset |
| The tester | [tools/fcn_tools/diag.py](../tools/fcn_tools/diag.py), [tools/fcn_tools/flasher.py](../tools/fcn_tools/flasher.py) | the connection factory, and `python -m fcn_tools.flasher` with its JSON report |
| The bench | [tools/fcn_tools/node.py](../tools/fcn_tools/node.py) (`Board`), [tools/fcn_tools/tester.py](../tools/fcn_tools/tester.py) (the tests import it as [tests/system/diag_bench.py](../tests/system/diag_bench.py)) | a node started again through fcn_boot after a requested reset; raw frames and raw requests for the system tests and the benches of bench/ |

The tester's two Python packages are independent implementations of both standards, and the
tests use them as the oracle for the node's C code; where a case needs what they never send (a
wrong sequence number, a request udsoncan would refuse to build), the tests write the frames or
the request bytes themselves.

**Why the bootloader has no UDS server.** The node is an A/B design: the running image writes
the slot that does not run, through lib/boot's slot writer, and keeps running while it does; the
bootloader only selects, validates and starts. A single-bank design has to receive the new image
in its bootloader, because the image being replaced cannot run while its own flash is erased, so
that bootloader carries CAN, ISO-TP and UDS. Here the bootloader stays small (lib/flash,
lib/image, lib/boot and the hand-off), which is what makes it reasonable never to update it
([update.md](update.md#what-a-product-would-add)). The image that received the download stays as
the fallback until the new one is confirmed. The price is a second slot of flash.

Clause numbers of the two standards are not cited: the rules are named by the titles of their
sub-clauses and tables, which were not checked against the text of the editions. Where a
standard leaves a choice, the headers mark the choice CHOICE, and so does this page.

## The transport: ISO-TP (lib/isotp)

[lib/isotp](../lib/isotp/isotp.h) is the transport for one connection: the tester sends on
0x7E0 (`UDS_Request`), the node answers on 0x7E8 (`UDS_Response`). It is pure C11 with no
operating system, no clock of its own and no allocation. The caller passes the time (a monotonic
microsecond count), hands it every received frame (`isotp_on_frame()`) and every transmit
confirmation of a frame it handed out (`isotp_on_tx_done()`), and polls it at the deadline it
names (`isotp_next_deadline()`, `isotp_poll()`). Frames leave through a callback (`send_frame`).
Reception and transmission are independent, as in the standard, so the node can take a request
while it is still sending a response. Normal addressing (the whole first byte of every frame is
the N_PCI), physical addressing, frames of 8 bytes.

### Frames

| Frame | Byte 0 | Byte 1 | Bytes 2..5 | Bytes 6..7 | Data bytes |
|---|---|---|---|---|---|
| SingleFrame (SF) | `0x0L`, L = SF_DL, 1..7 | data or padding | data or padding | data or padding | 1..7 |
| FirstFrame (FF) | `0x1H` | `LL`; FF_DL = `0xHLL`, 8..4095 | data | data | 6 |
| FF with the escape (received only) | `0x10` | `0x00` | FF_DL, 32 bits, most significant byte first | data | 2 |
| ConsecutiveFrame (CF) | `0x2N`, N = SN | data or padding | data or padding | data or padding | 1..7 |
| FlowControl (FC) | `0x3S`, S = FS: 0 ContinueToSend (CTS), 1 WAIT, 2 Overflow (OVFLW) | BS | byte 2 STmin, bytes 3..5 padding | padding | none |

- **Segmentation.** A message of 1..7 bytes is one SF; one of 8..4095 bytes an FF with its first
  6 bytes, then CFs of 7 bytes, the last one with what is left.
- **SN.** The FF counts as 0; the CFs carry 1, 2, ..., 15, 0, 1, ...
- **Length 0.** SF_DL 0 is the escape of CAN FD, so on classical CAN an empty message has no
  encoding: `isotp_send()` refuses length 0 and an SF with SF_DL 0 is ignored.
- **Padding.** Every frame the module sends has DLC 8, the bytes after the last meaningful one
  0xCC. An FC WAIT or OVFLW carries BS 0 and STmin 0.

**Received frames** are checked before they are acted on. A frame that fails a check changes
nothing but a counter (`ignored[]`, by reason) and does not end a reception in progress:

| Ignored | Reason (`isotp_ignored_t`) |
|---|---|
| another identifier or format, a remote frame, DLC above 8 | FOREIGN |
| DLC 0; an SF with DLC < 1 + SF_DL; an FF with DLC < 8; a CF with DLC < 1 + the data it must carry (7, or what is left for the last CF); an FC with DLC < 3 | SHORT |
| SF_DL 0 or 8..15; FF_DL 1..7 (FF_DLmin is 8); an escape FF whose FF_DL is 4095 or less | LENGTH |
| N_PCI type 0x4..0xF | RESERVED_TYPE |
| a CF no reception expects now; an FC no transmission waits for | UNEXPECTED_CF, UNEXPECTED_FC |

A frame whose DLC covers what its N_PCI needs is understood, padded or not, so a tester that does
not pad is served.

### Parameters and timers

| Parameter | `isotp_config_t` | The standard | What the module does |
|---|---|---|---|
| N_As | `n_as_us` | timeout, 1000 ms | sender: from the hand-out of an SF, FF or CF to its confirmation; runs out: N_TIMEOUT_A |
| N_Ar | `n_ar_us` | timeout, 1000 ms | receiver: from the hand-out of an FC to its confirmation; runs out: N_TIMEOUT_A |
| N_Bs | `n_bs_us` | timeout, 1000 ms | sender: from the confirmation of the FF or of the last CF of a block, or from an FC WAIT, to the next FC; runs out: N_TIMEOUT_Bs |
| N_Cr | `n_cr_us` | timeout, 1000 ms | receiver: from the confirmation of the FC CTS, or from a CF, to the next CF; runs out: N_TIMEOUT_Cr |
| N_Br | `n_br_us` | performance: N_Br + N_Ar < 0.9 × N_Bs | receiver: how long it waits for a buffer the layer above keeps before it sends WAIT (below) |
| N_Cs | `n_cs_us` (a target) | performance: N_Cs + N_As < 0.9 × N_Cr | sender: a CF goes out in the first call at or after the time STmin allows; measured: `n_cs_max_us`, `n_cs_late` |
| BS, STmin | `rx_bs`, `rx_stmin`; the sender takes them from each CTS | BS 0..255 (0: no further FC) | |
| N_WFTmax | `rx_wft_max` | the receiver's limit of WAITs in a row | |
| (none) | `tx_wft_max` | not in the standard | the sender's limit of WAITs in a row (below) |

A timer set at t with a value v runs out at t + v exactly, and every call acts on the timers that
have run out by its time before anything else: a frame that arrives at the very microsecond a
timer runs out is too late, one 1 µs earlier is in time.

### Flow control

| STmin byte | Separation time |
|---|---|
| 0x00..0x7F | 0..127 ms |
| 0x80..0xF0, 0xFA..0xFF | reserved: the sender uses 127 ms (counted in `stmin_reserved`) |
| 0xF1..0xF9 | 100..900 µs |

- **CTS.** The sender sends a block of BS CFs (all that are left for BS 0) and waits for the next
  FC; the values of each CTS apply to the block after it. STmin is counted from the confirmation
  of the previous CF, across a block boundary too (from the FC's arrival if the FC came before
  that confirmation); the first CF after the FF goes out at once. The receiver sends a CTS after
  the FF and after every `rx_bs` CFs, never after the last CF.
- **WAIT.** The receiver sends WAIT only while the layer above keeps the receive buffer, at most
  N_WFTmax in a row; one more would end the reception with N_WFT_OVRN. At the sender each WAIT
  starts N_Bs again, and more than `tx_wft_max` WAITs in a row end the transmission with
  N_WFT_OVRN: the standard bounds WAITs only at the receiver, so this bound is the module's
  addition, which keeps a receiver that never stops sending WAIT from holding the sender for
  ever.
- **OVFLW.** The receiver answers an FF whose FF_DL does not fit its buffer, and every escape FF,
  with OVFLW and receives nothing. A sender that receives OVFLW ends with N_BUFFER_OVFLW, also at
  a later block boundary.
- **Reserved FS (3..15).** The sender ends with N_INVALID_FS.

### Results, unexpected frames, confirmations

| N_Result (`isotp_result_t`) | Side | When |
|---|---|---|
| N_OK | both | the message went through |
| N_TIMEOUT_A | both | N_As or N_Ar ran out |
| N_TIMEOUT_Bs | sender | no FC within N_Bs |
| N_TIMEOUT_Cr | receiver | no CF within N_Cr |
| N_WRONG_SN | receiver | a CF with another SN than the expected one |
| N_INVALID_FS | sender | an FC with a reserved FS |
| N_UNEXP_PDU | receiver | an SF or FF arrived during a reception |
| N_WFT_OVRN | receiver; sender as the module's addition | N_WFTmax WAITs were not enough; more than `tx_wft_max` WAITs |
| N_BUFFER_OVFLW | sender | the receiver answered OVFLW |
| N_ERROR | both | the transmit path refused a frame (`send_frame` returned false) |

**Unexpected N_PDUs** follow the standard's table of their handling, with reception and
transmission independent: SF, FF and CF go to the receiver, FC to the sender. An SF during a
reception ends it with N_UNEXP_PDU and is received; an FF ends it and starts a new one; a CF is
processed only if the reception expects one now (its CTS is out and the block is not complete),
a wrong SN ending it with N_WRONG_SN, and is ignored otherwise; an FC is processed only while a
transmission waits for one.

**Confirmations** carry no identity of the frame: the caller confirms the frames in the order
they were handed out and never after their N_As or N_Ar ran out (the module has given such a
frame up). The order of a confirmation and the peer's answer is not required: on the node the
receive task (priority 7) can pass the tester's answer to the diagnostic task before the
transmit task (priority 4) passes the confirmation of the frame it answers, so an FC that
arrives before the confirmation of the FF or the last CF of a block, or a CF before that of the
CTS, is processed as if the confirmation had come with it (`proven`). At most 4 frames
(`ISOTP_PIPELINE_DEPTH`) are handed out and unconfirmed at a time.

**Statistics** (`isotp_stats_t`, 32-bit counters that wrap): requests and confirmations by
N_Result, indications by N_Result, frames out and in by kind, frames ignored by reason,
overflows, confirmations matched, unmatched, given up and proven, refused re-entrant calls, the
longest N_Br and N_Cs. The header's STATISTICS paragraph has the identities they keep, which the
unit tests assert after every step.

**Not in the module:** CAN FD; extended and mixed addressing (both identifiers can be 29 bits, so
normal fixed addressing is a choice of identifiers, but the node uses 0x7E0 and 0x7E8);
functional addressing; messages above 4095 bytes (answered with OVFLW); frames shorter than 8
bytes on transmission; changing BS and STmin at run time; half-duplex operation.

## The node's connection (app/diag.c, the diagnostic task)

The diagnostic task (priority 3, between `can_tx` and `health`) owns one lib/isotp instance and
the lib/uds server, joined by [app/diag.c](../app/diag.c), and the node's UDS backend
([app/diag_backend.c](../app/diag_backend.c)). [architecture.md](architecture.md) has the task
table, its budget and the schedulability check.

### The node's ISO-TP parameters

| Parameter | Value | Why |
|---|---|---|
| identifiers | 0x7E8 out, 0x7E0 in, 11 bits | [dbc/fcn.dbc](../dbc/fcn.dbc) |
| N_As, N_Ar, N_Bs, N_Cr | 1000 ms | the standard's values; N_As and N_Ar equal the transmit queue's event timeout (`TX_EVENT_TIMEOUT_MS`, [app/can_io.c](../app/can_io.c)) |
| BS | 16 | bounds what the queue to the diagnostic task holds (below) |
| STmin | 0 | the receive FIFO is drained by the highest-priority task, and the queue is bounded by BS |
| receive buffer | 1026 bytes (`UDS_REQUEST_MAX`) | the longest request lib/uds takes, a TransferData of 1024 bytes of data; a longer message gets FC OVFLW (`32 00 00 CC CC CC CC CC`) |
| N_WFTmax, N_Br | 0 | the request goes to `uds_request()` inside the indication, which copies what it keeps; the receive buffer is never kept, so the node never sends FC WAIT |
| WAITs accepted from the tester | 4 in a row (`DIAG_TX_WFT_MAX`) | a tester not ready for a response |
| N_Cs target | 150 ms | at least the longest STmin a tester can ask for (127 ms) |
| response buffers | 2 | lib/isotp does not copy: a response produced while another is sent (the final response after a responsePending) waits in the second; one that finds both in use is dropped and counted (`responses_dropped`) |

On the wire: the node's flow control is `30 10 00 CC CC CC CC CC`, every frame is padded with
0xCC, and a responsePending is `03 7F <SID> 78 CC CC CC CC`. lib/isotp hands out a consecutive
frame only after the confirmation of the one before (STmin counted from it), so a response
leaves one frame per transmit-done report.

### How the frames reach the task, and leave it

| Path | Mechanism |
|---|---|
| UDS_Request 0x7E0: bus → `can_rx` → `diag` | `can_rx` puts every 0x7E0 frame into a FreeRTOS queue of 18 frames (`APP_DIAG_RX_QUEUE_LENGTH`) and notifies the diagnostic task; it never waits: a frame the queue refuses is lost and counted (`diag.task.rx_lost`) |
| UDS_Response 0x7E8: `diag` → `can_tx` → bus | an event frame of the transmit queue (`can_io_send_event_stamped()`, which returns the time the frame was queued with); `can_tx` is woken only when no other event frame is held, otherwise the report of the frame ahead loads it (`can.driver.tx_quiet_enqueues`) |
| transmit-done of 0x7E8: `can_tx` → `diag` | `can_io_on_tx_done()`: every report of an event frame with 0x7E8, in report order, with the time it was queued with, into a queue of 8 (`APP_DIAG_TX_DONE_QUEUE_LENGTH`); refused reports are counted (`diag.task.tx_done_lost`) |
| a confirmation into lib/isotp | `diag_on_tx_done()` passes it only if it is for the oldest frame handed out, and only while that frame's N_As has not run out; older records are of frames the transmit queue discarded (`confirms_forgotten`), late and unknown reports are dropped (`confirms_late`, `confirms_unknown`), so a late confirmation is never taken for the next frame |

The task takes the reports first, then the frames, at most 2 of them in a tick
(`APP_DIAG_EVENTS_PER_TICK`), then polls lib/isotp and lib/uds, and waits until the connection's
next deadline (lib/isotp's timers, lib/uds's responsePending and S3server), at most 50 ms; every
pass gives its alive indication. The cap of 2 events per tick is what keeps the task's demand
bounded when a tester floods 0x7E0 (the analysis in [architecture.md](architecture.md)); its
price is throughput: a block of 1024 bytes is 147 frames and the reports of the node's flow
controls, at least 79 ticks.

### Why no consecutive frame is lost in the node

- **The protocol bounds the queue.** A tester that keeps to ISO 15765-2 sends at most one block
  of 16 consecutive frames before it waits for the next flow control, which the node sends only
  once the diagnostic task has taken the block's 16th frame out of its queue; between two
  messages the tester waits for the node's response. So the queue holds at most 16 of its frames,
  however late the diagnostic task runs; it has 18 places.
- **The receive FIFO is drained** by `can_rx`, the highest-priority task, in batches of 32
  ([architecture.md](architecture.md), phase 1's bug 007).
- **Losses are counted:** `can.driver.rx_fifo_overruns`, `can.controller.wire_overruns` (the bus
  model), `diag.task.rx_lost` and `diag.task.rx_high_water`, and lib/isotp's `rx_results` (a lost
  consecutive frame shows as WRONG_SN or TIMEOUT_CR).
- **Tested under load:** `test_no_frame_is_lost_in_the_node_under_a_flood_of_lower_priority_frames`
  ([test_download.py](../tests/system/test_download.py)) floods the bus with 8-byte frames of
  0x7F0 at 0.4 of the bus's capacity during a whole download, on top of the node's full telemetry;
  in one run of `pytest tests/system/test_download.py -k flood -s` against the release build,
  40,334 flood frames went out, and every one of the tester's frames reached lib/isotp.
- **The bus model's limit.** The bus model keeps other nodes' frames in the order the transport
  delivers them, not in their priority ([can-simulation.md](can-simulation.md)), so a flood above
  the bus's capacity makes it drop the tester's frames with the flood's (`wire_overruns`), where a
  real bus would let the higher-priority 0x7E0 frames win the arbitration. That is a limit of
  the model, not of the node, and why the flood test stays at 0.4 of the capacity.
- **What the flood test needs from the host.** At 0.4 the flood and the tester's frames (about
  1.7 per ms of the 3.7 the bus carries) keep the bus close to full, so the model's queue of the
  other nodes' frames (64 frames, `CAN_CTRL_WIRE_QUEUE`) has little room to spare, and the host
  transport stamps every frame of one read pass with the same time: a host that holds up the
  node's reads of its bus bunches the flood and a block of the tester into one pass, and a
  frame of the tester can be dropped with the flood's. Measured with a stall planted in the
  node's CAN thread of a scratch copy, every 2 s from the fourth second, about ten stalls in a
  download (`pytest tests/system/test_download.py -k flood`, release build, the UDP bus): stalls
  of 10 ms (1 run), 20 ms (3 runs) and 25 ms (3 runs) left every frame; stalls of 30 ms dropped
  3, 5 and 6 frames in 3 runs, stalls of 40 ms 23 and of 60 ms 59 (1 run each), and each of
  those downloads failed. So the test needs the node's reads of the bus to come no more than
  about 25 ms apart for the whole download. The node counts what the model dropped, and the
  test is marked `timing`: with a wire overrun the run is a missed timing tolerance
  (`check_timing`), with none every count is asserted. The flood itself never bursts:
  `update_bench.Flood` gives up the frames a delay of its thread cost it (a planted 80 ms stall
  of that thread then dropped none; when it still made them up in a burst, as the flood of the
  test did before, the same stall dropped 74 and failed the download).

### The flash jobs and their supervision

| Job | Service | lib/boot | Result |
|---|---|---|---|
| erase | RoutineControl 0xFF00 | `boot_slot_by_address()`, `boot_writer_begin()`, then `boot_writer_erase_step()` until not BUSY | the last call's status |
| write | TransferData | `boot_writer_write()` of the block (lib/uds's copy of the request, unchanged until the job is done) | its status |
| exit | RequestTransferExit | the first `boot_writer_finish_step()`: the last partial page, the start of the validation | BOOT_BUSY when it succeeds |
| check | RoutineControl 0xFF01 | `boot_writer_finish_step()` until not BUSY: the validation (one step per 64 bytes), then the COMMIT record | its status |
| confirm | RoutineControl 0xF000 | `boot_confirm()` of the hand-off's slot, if the node runs as a trial | BOOT_E_STATE without a trial |

- **One job per service step**, not one per lib/boot call: the flash task loops lib/boot's steps
  itself, at its own priority. There is one mailbox of one job, because lib/uds runs one
  operation at a time; the flash task notifies the diagnostic task when the job is done, and
  lib/uds answers responsePending meanwhile. Without a flash every job ends with BOOT_E_STATE
  (conditionsNotCorrect, 0x22).
- **Log lines:** `download: slot at 0x102000 erased for <n> bytes, <steps> steps (<us> us)`,
  `download: the image validated and is committed as the trial, <steps> steps (<us> us)`,
  `confirmed by the tester's confirm routine: slot B, image x.y.z, is the active image (<us> us)`,
  and on a failure `W app: flash job <kind>: <status> after <steps> steps (<us> us)`.
- **Supervision.** The flash task is a supervised entity of the watchdog manager. It gives an
  alive indication after every wait (it waits at most 100 ms) and after every lib/boot call in a
  job, and is examined every 6 supervision cycles (600 ms) with a failed tolerance of 1: a record
  at the part's maximum times takes 409 ms, so a working flash task gives an indication in every
  window, and one that stops makes its entity EXPIRED, after which the hardware watchdog resets
  the node 500 ms after the last kick (SUPERVISION in [app/app.c](../app/app.c),
  [faults.md](faults.md)). A download stuck in the flash task therefore ends in a watchdog reset
  (status 11), never in a hang; the tester sees responsePending and then silence, and the next
  start runs the confirmed image (`test_a_flash_job_that_never_returns_ends_in_a_watchdog_reset`,
  with `--flash-hang 1`).

### The confirm: the routine and the self-confirm

Both run in the flash task, one after the other, and the first one that succeeds writes the
CONFIRM record:

- **The self-confirm first** (the trial's 5 s of good health, [update.md](update.md)): the routine
  then finds no trial to confirm and is refused (0x22) and writes nothing. The flasher accepts
  that only if DID 0xFD00 then says CONFIRMED.
- **The routine first:** the self-confirm writes nothing and answers BOOT_OK
  (`confirm.flash_task.self_skipped`); the health task's monitor becomes CONFIRMED with
  `by_routine` (`update_routine_confirmed()`, log line `the trial in slot B is confirmed by the
  tester's confirm routine`).
- DID 0xFD00's confirm state is CONFIRMED as soon as the flash task has confirmed, before the
  health task's next period. The routine also proves what the 5 s window cannot: that the trial
  receives.
- **In the tests.** Which of the two comes first is a race between the tester and the trial's
  window. A test that checks the routine's way gives the trial a window of an hour with the host
  node's `--confirm-periods 36000`, a test hook the product does not have
  ([update.md](update.md), the confirm), so that only the routine can confirm it;
  `test_a_trial_no_tester_confirms_confirms_itself_and_the_routine_then_finds_none` checks the
  self-confirm first, with the product's window.

### The DTC services and the reset

- **The snapshot.** After every period the health task copies lib/dtc's state, with the count of
  clears it has carried out, in a critical section; the backend copies it out the same way. A
  read describes the end of the health task's last period: at most one period and the health
  task's response time (45 ms by the analysis) old.
- **The clear.** ClearDiagnosticInformation adds one to a count of clear requests, which is added
  to TESTER_Command's; the health task carries out every new sum as it does for TESTER_Command,
  in order with the evidence ([faults.md](faults.md)), and notifies the diagnostic task; the
  positive response waits until the count carried out has reached the request's. Log line:
  `DTCs cleared: ClearDiagnosticInformation (clear <n>)`.
- **ECUReset.** The diagnostic task makes the reset once the transmission of the positive
  response has ended, N_OK or not, or 2 s (2 x N_As) after it took the request if that comes
  first; with the SPRMIB at once. ISO 14229-1 asks for the positive response before the reset
  and sets no time for a hardReset. The deadline is the node's: a response that waits behind
  another waits for the whole of that transmission, whose length the tester's flow control sets
  (up to four WAITs per block, blocks of one frame, STmin), and a reset the tester asked for is
  not to be held off by the tester's own flow control. For a tester that waits for each
  response the deadline never comes first, and the reset waits for its `51 01`: ahead of it
  there is at most the last frame of the previous response, which the tester has received, so
  that transmission ends within N_As of the request, and the `51 01` within N_As of its
  hand-out. A tester that sends `11 01` while it still receives an earlier response may get the
  reset with that response cut and no `51 01`
  ([test_download_faults.py](../tests/system/test_download_faults.py) holds one with WAITs and
  blocks of one frame), or, if the earlier one ends just before the deadline, with its `51 01`
  cut. The reset is `plat_reset(PLAT_RESET_REQUESTED)`: exit status 10 on the host, after the
  log line `W app: ECUReset: resetting (requested by the tester)`. A run that ends in a reset
  writes no statistics.

### Statistics (`--stats-json`, member "diag")

- `session`; `task`: `events`, `passes`, `budget_waits`, `events_per_tick`, `rx_queue_length`,
  `rx_lost`, `rx_high_water`, `tx_done_queue_length`, `tx_done_lost`.
- `connection`: `frames_in`, `frames_out`, `frames_refused`, `confirms`, `confirms_passed`,
  `confirms_late`, `confirms_unknown`, `confirms_forgotten`, `requests`, `responses`,
  `responses_sent`, `responses_failed`, `responses_dropped`, `resets`.
- `isotp`: `parameters` (`tx_id`, `rx_id`, `bs`, `stmin`, `rx_buffer`, `timeout_us`, `n_br_us`,
  `n_cs_us`, `rx_wft_max`, `tx_wft_max`, `pipeline`) and lib/isotp's counters, the results, frames
  and ignored frames by name.
- `uds`: `requests` by service name, `requests_unsupported`, `requests_empty`, `nrc` by NRC name,
  `response_pending`, `positive`, `suppressed`, `session_changes`, `s3_timeouts`, `resets`,
  `pending_max_us`, and `download` (`state`, `address`, `size`, `received`, `blocks`,
  `repeated_blocks`, `counter`, `erases`, `started`, `exited`, `committed`, `aborted`, `failed`,
  `erase_us`, `transfer_us`, `check_us`).
- `backend`: `post_refused`, `snapshots`, `clears`, `clears_done`, `resets`, and per job kind
  `posted`, `failed`, `last_steps`, `last_status`.
- Also: `can.driver.tx_done_reports` and `tx_quiet_enqueues`; in `confirm.flash_task`,
  `confirmed_by_routine`, `self_skipped`, `idle_passes`, and per job kind (`jobs`) `run`,
  `last_status`, `last_steps`, `last_ns`, `max_ns` and (since phase 6) `total_ns`, the sum of
  the kind's times, from which bench/isotp_throughput.py takes the mean time of a write job.

## The UDS server (lib/uds)

[lib/uds/uds.h](../lib/uds/uds.h) is pure C11 without an operating system, allocation, globals
or a clock. A complete request comes in as bytes with the time its last byte arrived
(`uds_request()`); the server answers with the bytes of one response, or none, written into a
buffer of the caller's; `uds_poll()` carries on a request whose work is still pending. Everything
the node does for a service goes through a backend of nine functions (`uds_backend_t`), each of
which can answer *pending*.

### Sessions

| Session | Value | Entered from | Services in it |
|---|---|---|---|
| defaultSession | 0x01 | every session, at the start, and by S3server | 0x10, 0x11, 0x14, 0x19, 0x22, 0x3E |
| programmingSession | 0x02 | extended, programming | 0x10, 0x11, 0x22, 0x31 (0xFF00, 0xFF01), 0x34, 0x36, 0x37, 0x3E |
| extendedDiagnosticSession | 0x03 | default, extended | 0x10, 0x11, 0x14, 0x19, 0x22, 0x31 (0xF000), 0x3E |

A transition the table does not have (default to programming, programming to extended) is
answered with subFunctionNotSupportedInActiveSession (0x7E). CHOICE: programming is entered from
extended, as the programming sequence does, and left only for default. Every accepted
DiagnosticSessionControl starts its session afresh, also when it names the active one: a download
in progress is abandoned and the routines' results are forgotten. lib/boot makes an abandoned
download safe: the slot stays WRITING, which the bootloader never starts, and the next
eraseMemory begins it again.

### Timing

| Parameter | Value | What the server does |
|---|---|---|
| P2server_max | 50 ms | reported in the DiagnosticSessionControl response as 0x0032 (1 ms resolution) |
| P2*server_max | 5000 ms | reported as 0x01F4 (10 ms resolution) |
| first responsePending | 40 ms after the request | a request still pending then is answered with `7F <SID> 78` |
| next responsePending | 2000 ms after the one before | until the final response |
| S3server | 5000 ms | a non-default session with no request for 5000 ms since the last response falls back to the default session |

- Times count from the `now_us` of the call that delivered the request. A time earlier than one
  the server has seen counts as that time.
- **responsePending.** The first leaves 10 ms of P2server for the diagnostic task's latency and
  the frame; each next one counts from the one actually produced, so two are never less than
  2000 ms apart and each comes 3000 ms before the tester's P2*server would run out. A backend that
  finishes before 40 ms causes none: at each poll the server polls the backend first and looks at
  the clock after.
- **Long operations.** With a responsePending every 2000 ms no flash operation is too long for the
  tester: a sector erase at the part's maximum of 400 ms (W25Q128JV,
  [update.md](update.md#timing)) or a whole-slot erase of 256 sectors only adds responsePending
  messages. What must be bounded is the diagnostic task's poll: it polls when the flash task
  reports a job done, and no later than `uds_next_deadline()`.
- **S3server** runs from the call that produced the last request's final response (or would have,
  had it not been suppressed), and expires at exactly 5000 ms after it. It does not run in the
  default session nor while a request is pending. Every request restarts it, refused ones too.
  CHOICE: it starts at the response the server hands out, not at the transport's confirmation;
  the difference is the transmission of one response. `uds_request()` checks S3server before
  the request, so a request after the timeout with no poll in between finds the default session.

### The general response behaviour

Every request goes through these checks in this order; the first that fails decides the
negative response `7F <SID> <NRC>`, and no later check is made:

| Step | Check | NRC |
|---|---|---|
| 1 | no earlier request is still pending | 0x21 busyRepeatRequest |
| 2 | the SID is one of the server's ten services | 0x11 serviceNotSupported |
| 3 | the service is supported in the active session | 0x7F serviceNotSupportedInActiveSession |
| 4 | services with a sub-function: the request has at least 2 bytes | 0x13 incorrectMessageLengthOrInvalidFormat |
| 5 | services with a sub-function: the sub-function is supported (bit 7 removed first where it is the SPRMIB) | 0x12 subFunctionNotSupported |
| 6 | services with a sub-function: the sub-function is supported in the active session | 0x7E subFunctionNotSupportedInActiveSession |
| 7 | the service's own checks, in the order of its table below | per service |
| 8 | the positive response fits the caller's buffer | 0x14 responseTooLong |

This is the order of the standard's general server response behaviour; its SecurityAccess and
Authentication steps fall away, since neither service is implemented. A request of 0 bytes is
ignored (`requests_empty`). On the node, step 8 cannot fail: the diagnostic task's buffers have
`UDS_RESPONSE_MAX` bytes (131), the longest response.

**A request while one is pending.** CHOICE: one request at a time. A request that arrives while
one is pending is answered with busyRepeatRequest at once, whatever it is (an unknown SID too),
and is not kept; the pending request goes on undisturbed, its responsePending cadence included.

**suppressPosRspMsgIndicationBit**, bit 7 of the sub-function of DiagnosticSessionControl,
ECUReset, RoutineControl and TesterPresent. With it set the service is carried out (the session
changes, the reset is made, the routine runs) and the positive response is not sent; a negative
response is sent as always; and the final positive response of a request for which a
responsePending went out is sent all the same, because the tester now waits for one. CHOICE:
ReadDTCInformation gets no such bit: its whole sub-function byte is the reportType, so 0x81, 0x82
and 0x8A are sub-functions not supported (0x12).

**Responses.** A positive response's SID is the request's plus 0x40; multi-byte parameters are
big-endian.

### The services

The NRCs listed are the service's own checks (step 7) in their order; steps 1 to 6 come before.

| Service | Request | Positive response | Sub-functions | Sessions | Its checks, in order |
|---|---|---|---|---|---|
| DiagnosticSessionControl 0x10 | `10 <session>` | `50 <session> 00 32 01 F4` | 0x01, 0x02, 0x03 (SPRMIB) | all | 0x13 not 2 bytes |
| ECUReset 0x11 | `11 01` | `51 01`, then the reset | 0x01 hardReset only (SPRMIB) | all, in every state of a download | 0x13 not 2 bytes; then the backend (the node's never refuses) |
| TesterPresent 0x3E | `3E 00` | `7E 00` | 0x00 (SPRMIB) | all | 0x13 not 2 bytes |
| ReadDataByIdentifier 0x22 | `22 <DID>`, one DID | `62 <DID> <data>` | | all | 0x13 not 3 bytes (also more than one DID); 0x31 a DID the server does not have; for 0xF189 0x22 without a hand-off |
| ReadDTCInformation 0x19 | `19 01 <mask>`, `19 02 <mask>`, `19 0A` | below | 0x01, 0x02, 0x0A (no SPRMIB) | default, extended | 0x13 not 3 bytes (0x01, 0x02) or 2 bytes (0x0A); the backend; 0x10 for a backend that gives no snapshot |
| ClearDiagnosticInformation 0x14 | `14 FF FF FF` | `54`, once the clear has been made | | default, extended | 0x13 not 4 or 5 bytes; 0x31 a group other than 0xFFFFFF, or a fifth byte (a MemorySelection); then the backend |
| RoutineControl 0x31 | `31 01 <RID> [option record]`, `31 03 <RID>` | `71 01 <RID> 00 00`, `71 03 <RID> <outcome> <NRC>` | 0x01 startRoutine, 0x03 requestRoutineResults (SPRMIB); 0x02 stopRoutine: 0x12 | extended, programming | 0x13 fewer than 4 bytes; 0x31 a routine the server does not have or not in this session; then the routine's |
| RequestDownload 0x34 | `34 00 44 <address, 4> <size, 4>` | `74 20 04 02` | | programming | 0x13 fewer than 3 bytes; 0x31 addressAndLengthFormatIdentifier not 0x44; 0x13 not 11 bytes; 0x31 dataFormatIdentifier not 0x00; 0x22 a download in TRANSFER; 0x70 no eraseMemory completed for a download; 0x31 another address or size than the erase's |
| TransferData 0x36 | `36 <counter> <1..1024 bytes>` | `76 <counter>` | | programming | 0x13 fewer than 2 bytes; 0x24 no download in TRANSFER; 0x31 longer than 1026 bytes; then the counter of the last block accepted is a repeat: positive, not written; 0x24 every byte taken already; 0x73 not the next counter; 0x13 no data; 0x71 more data than the size leaves (ends the download); then the write, whose refusal ends the download |
| RequestTransferExit 0x37 | `37` | `77` | | programming | 0x13 not 1 byte; 0x24 no download in TRANSFER, or not every byte taken (the download goes on); then the first finish step, whose refusal ends the download |

**DIDs.** CHOICE: one DID per request.

| DID | Data |
|---|---|
| 0xF186 ActiveDiagnosticSessionDataIdentifier | 1 byte: the active session |
| 0xF189 vehicleManufacturerECUSoftwareVersionNumberDataIdentifier | the running image's version from the bootloader's hand-off, ASCII `major.minor.patch` in decimal, no leading zeros, no terminator: 5 to 13 bytes. Without the bootloader there is no hand-off and no image header, so the answer is 0x22 conditionsNotCorrect, not a number that is not the image's |
| 0xFD00 boot information (system-supplier range) | 12 bytes, layout version 1, below |

| Byte | Field | Values |
|---|---|---|
| 0 | layout version | 1 |
| 1 | hand-off | 0 none (started without the bootloader), 1 taken |
| 2 | the slot started | 0 A, 1 B; 0xFF without a hand-off |
| 3 | boot mode | 1 CONFIRMED, 2 TRIAL; 0 without a hand-off |
| 4 | attempt | 1 to 3 for a trial; 0 for the confirmed image or without a hand-off |
| 5 | confirm state | BMS_BootInfo's ConfirmState ([app/update.h](../app/update.h)): 0 NOT_NEEDED, 1 WAITING, 2 CONFIRMING, 3 CONFIRMED, 4 FAILED |
| 6 | the bootloader's event | lib/boot's values: 0 NONE, 1 ADOPT, 5 ROLLBACK, 6 REJECT, 7 FALLBACK; 0 without a hand-off |
| 7 | the event's slot | 0, 1, or 0xFF for none |
| 8 to 11 | image version | big-endian, `major << 24 \| minor << 16 \| patch`; 0 without a hand-off |

**ReadDTCInformation's answers.** `59 01 FF 01 <count, 2 bytes>`; `59 02 FF { <DTC, 3 bytes>
<status> }`; `59 0A FF { <DTC, 3 bytes> <status> }`. The third byte is the
DTCStatusAvailabilityMask, 0xFF (lib/dtc implements every bit of the status byte,
[faults.md](faults.md)); the fourth of the 0x01 answer is the DTCFormatIdentifier 0x01,
ISO_14229-1_DTCFormat. A DTC matches a mask if (statusOfDTC & mask) ≠ 0, so a mask of 0 matches
nothing; 0x02 lists the DTCs that match in the order of the fault table, 0x0A every DTC of the
table whatever its status, 0x00 included. With the table's maximum of 32 DTCs
(`DTC_EVENTS_MAX`) a list is 3 + 4 × 32 = 131 bytes, the server's longest response; the node's
table has 17, so its 0x0A answer is 71 bytes. The answer is built from the health task's snapshot
with lib/dtc's read functions within one call. ClearDiagnosticInformation's backend answers
pending until the health task has made the clear, so the positive response never comes before
it, and the tester gets responsePending if the clear takes longer than 40 ms.

**The routines** (the routine status record, `UDS_ROUTINE_STATUS_SIZE`, is 2 bytes: the outcome,
0x00 completed or 0x01 ended with a negative response, then that response's NRC):

| RID | Routine | Session | startRoutine | Its checks, in order | Its work |
|---|---|---|---|---|---|
| 0xFF00 | eraseMemory | programming | `31 01 FF 00 44 <address, 4> <size, 4>` | 0x13 no option record; 0x31 addressAndLengthFormatIdentifier not 0x44; 0x13 not 13 bytes; 0x22 a download in TRANSFER | the erase job |
| 0xFF01 | checkProgrammingDependencies | programming | `31 01 FF 01` | 0x13 not 4 bytes; 0x24 no download in EXITED | the check job: the validation and the COMMIT record that makes the slot TRIAL |
| 0xF000 | confirm the running trial (system-supplier range) | extended | `31 01 F0 00` | 0x13 not 4 bytes | the confirm job; no trial to confirm: 0x22 |

requestRoutineResults (`31 03 <RID>`, exactly 4 bytes, else 0x13) reports the last start of the
routine that ran in the active session, and answers 0x24 if none did; a start refused by its
routine's checks did not run and leaves no result, and the results are forgotten when a session
begins. It is how a tester whose response to a long erase or check was lost finds out how it
ended.

### The download

**The state machine** (`uds_dl_state_t`, also `uds_stats_t.download.state`): IDLE → ERASED (A, N)
by an eraseMemory that completes → TRANSFER by RequestDownload (A, N) → TransferData until N bytes
→ EXITED by RequestTransferExit → CHECKED by the check routine (the slot is TRIAL).
RequestDownload calls no backend: eraseMemory has begun the slot and erased it for exactly this
address and size, and RequestDownload checks that and starts the transfer.

| Event | In | Next | Counted in |
|---|---|---|---|
| eraseMemory | TRANSFER | refused with 0x22; the download goes on | |
| eraseMemory | any other state | IDLE until the erase completes (then ERASED); IDLE if refused | `aborted`, from EXITED |
| a refusal of a write, the exit or the check; transferDataSuspended (0x71) | TRANSFER, EXITED | IDLE | `failed` |
| an accepted DiagnosticSessionControl; S3server | any | IDLE | `aborted`, from TRANSFER or EXITED |
| a wrong counter, a malformed block, an exit before the last byte, a check before the exit | any | unchanged | |

In every one of these cases lib/boot leaves the slot WRITING, which the bootloader never starts.
The next eraseMemory writes no BEGIN record for it again and erases every sector the new image
needs.

**The block counter** starts at 0x01 and wraps from 0xFF to 0x00; the data go to
`boot_writer_write()` at the offset the blocks so far add up to. A block whose counter is that of
the last block accepted is answered positively and not written again (the tester repeats a block
whose response it lost); its data are not compared. The rule comes before "every byte taken", so
a repeat of the last block is accepted after the download is complete.

**maxNumberOfBlockLength: 1026** (the SID, the counter and 1024 bytes of data), CHOICE, at the
minimum of 1024 data bytes:
- **RAM on a Cortex-M3.** The node holds a block twice, in lib/isotp's receive buffer and in the
  server's copy of the request in progress (`UDS_REQUEST_MAX`), which it keeps so that the
  transport's buffer is free for the next message while the flash task writes the block. 2 × 1026
  bytes are 10 % of an assumed budget of 20 KiB of SRAM, that of a small Cortex-M3 part such as
  the STM32F103C8 (a datasheet figure, not this project's target); the largest block a classic-CAN
  first frame can announce, 4095 bytes, would take 40 %. For comparison, built for the Cortex-M3
  (arm-none-eabi-gcc 13.2.1, `-mcpu=cortex-m3 -mthumb -Os`), `uds_server_t` is 1392 bytes and
  `isotp_t` 472.
- **The round trip per block.** A block of 1024 data bytes is a first frame and 146 consecutive
  frames, 147 frames of 108 to 132 bits plus 3 of intermission ([lib/can/can_bits.h](../lib/can/can_bits.h)):
  32.6 to 39.7 ms of bus time at 500 kbit/s (the blocks bench/isotp_throughput.py sent, with
  their 10 flow controls and the response, take 36.7 to 37.3 ms, 36.8 ms on average, by their
  exact bit counts, a computed figure). Each block also costs a round trip: the node's flow
  controls, the write job (four page programs: 1.6 ms typical, 12 ms at the part's maximum, on
  the part; on the host each program's wait ends at the next tick, so the job takes longer, as
  bench/isotp_throughput.py measures: 3.97 ms over SocketCAN and 3.96 ms over UDP on average in
  its final run, a host time), the
  response and the tester's next request. Blocks of 2048 bytes would halve the round trips (128
  instead of 256 for a 256 KiB image). Phase 6 does not rebuild the node to try it:
  [bench/isotp_throughput.py](../bench/isotp_throughput.py) measures the blocks of 1 to 1024
  bytes, cuts each into its parts, and projects from them what blocks of 2048 bytes would save
  (a projection, labelled so in [results/isotp_throughput.json](../results/isotp_throughput.json)).
  The final run's projection saves 0.186 s over SocketCAN and 0.176 s over UDP of the 21.2 s
  that a 256 KiB image takes in blocks of 1024 bytes, 0.9 % and 0.8 %: a block's time is
  almost all its events at the budget's rate (the line fitted to the measured sizes: 0.452 ms
  per block and 0.499 ms per event over SocketCAN, against the budget's 0.5 ms), and a block of
  2048 bytes has 312 events, two fewer than two blocks of 1024.
- **Whole pages.** 1024 bytes are four 256-byte pages, so every block but the last programs whole
  pages and nothing waits in the slot writer's page buffer between blocks.

**lib/boot's status as an NRC** (`uds_nrc_for_boot()`), the table [update.md](update.md) proposed:

| lib/boot | NRC | When |
|---|---|---|
| BOOT_OK | none | |
| BOOT_BUSY | none at RequestTransferExit, whose first finish step ends in it; 0x72 elsewhere | an operation that must end in BOOT_OK did not |
| BOOT_E_ARG, BOOT_E_SIZE | 0x31 requestOutOfRange | no slot starts at the address; a size of 0 or beyond the slot |
| BOOT_E_STATE | 0x22 conditionsNotCorrect | the running (CONFIRMED) slot; a trial pending (a download would overwrite its fallback); the factory state; no trial to confirm |
| BOOT_E_SEQUENCE | 0x24 requestSequenceError | the slot writer called out of order |
| BOOT_E_INVALID, BOOT_E_FLASH, BOOT_E_POWER_LOST, any other | 0x72 generalProgrammingFailure | the image did not validate; the flash refused an operation |

The server's own checks add 0x70, 0x71 and 0x73 where the standard asks for them. The backend's
functions and how the server calls them are in uds.h (THE BACKEND); an NRC a backend returns that
the server does not name is sent as generalReject (0x10). The server's statistics
(`uds_get_stats()`) are the `uds` member above.

## The programming sequence and the tester

**The connection factory.** `fcn_tools.diag.connect(spec)` opens a udsoncan client over
can-isotp's NotifierBasedCanStack on python-can, for `socketcan:vcan0` or `udp:<port>`. ISO-TP:
0x7E0 and 0x7E8, padding 0xCC, the tester's own flow control with BS 0 and STmin 0, `wftmax` 0
(the node never sends WAIT). UDS timing: P2client 1 s, P2*client 6 s (the node's P2*server and a
second for the hosts), never the server's own figures (the flasher's wait for the trial, below,
polls with the node's P2server instead); request timeouts from the part's maximum
times: eraseMemory sectors × 400 ms + 409 ms + 10 s, the check (size / 64 + 8) × 0.1 ms + 409 ms +
10 s (0.1 ms per validation step on a Cortex-M3 is an assumed design figure), a block (pages + 1)
× 3 ms + 10 s, anything else 10 s.

**The flasher**, `python -m fcn_tools.flasher --can SPEC IMAGE [--report PATH] [--address A]
[--no-reset] [--no-confirm] [--trial-wait-s S]`, makes the sequence step by step and times each:

| Step | Request | Answer |
|---|---|---|
| boot_info | `22 FD 00` | the running slot: the target is the other one (or `--address`) |
| default_session | `10 01` | `50 01 00 32 01 F4`: the sequence starts in the default session, whatever a tester before left (programming is entered from extended only) |
| extended_session | `10 03` | `50 03 00 32 01 F4` |
| programming_session | `10 02` | `50 02 00 32 01 F4` |
| erase | `31 01 FF 00 44 <slot> <size>` | responsePending, then `71 01 FF 00 00 00` |
| request_download | `34 00 44 <slot> <size>` | `74 20 04 02`: maxNumberOfBlockLength 1026 |
| transfer | `36 <counter> <1024 bytes>`, ... | `76 <counter>`, ... |
| transfer_exit | `37` | `77` |
| check | `31 01 FF 01` | `71 01 FF 01 00 00` (responsePending first if it takes longer than 40 ms) |
| ecu_reset | `11 01` | `51 01`, then the reset; fcn_boot starts the trial as attempt 1 |
| wait_for_trial | `22 FD 00` until it names the target slot as a trial: the first 50 ms after the `51 01`, each waiting 50 ms, the next 50 ms after one that did not find it | an answer of the image before the reset does not count |
| read_version | `22 F1 89` | the trial's version |
| extended_session_trial | `10 03` | |
| confirm | `31 01 F0 00` | `71 01 F0 00 00 00`, or `7F 31 22` when the trial confirmed itself first |
| boot_info_confirmed | `22 FD 00` | confirm state CONFIRMED |

`<slot>` is the start of the slot that is not running: on the host slot A is 0x00002000 and slot
B 0x00102000. `<size>` is the image's size, header included. `--no-reset` stops after the check
(the trial is committed, the node keeps running the old image), `--no-confirm` after
read_version. On stderr each step prints `<name> <duration> s ok|FAILED <nrc name|error>`, then
`error: <error>` if the flasher stopped between two steps or could not close the bus after
them, and `result: <result>`. The report
(schema `fcn.flash_report/1`) has the tool and package versions, the bus, the image (path, size,
version, target, SHA-256), the slot, address, maxNumberOfBlockLength and data per block, the
boot information before, of the trial and after, the routines' status records, who confirmed
(`routine` or `node`), the transfer's bytes, blocks, duration and throughput, every step (name,
request, start, duration, ok, and the NRC or the error: `timeout: ...`, `unexpected response:
...` or `invalid response: ...` for an answer udsoncan rejects, the type and text of an
exception of python-can or an OSError), the time of every block, and `result` (`confirmed`,
`committed`, `trial` or `failed`) with the failed step, or, after an error of the flasher between
two steps, no failed step and `error` (`error` also gives an error closing the bus after the
sequence, which fails the run as well). Exit status: 0; 1 when the flasher cannot start, before
its first request and without a report: an argument it refuses (not argparse's own 2), a report
file it cannot write, an image it cannot read or that is no image, a bus it cannot open (the
report file is opened first, and removed again after these last two); 2 when the sequence
failed, with its report: a step failed (a negative response, a timeout, an answer udsoncan
rejects or the flasher cannot use, an error of the bus or the transport), or the flasher stopped
between two steps on an error of its own, or it could not close the bus after the sequence.

**wait_for_trial** is the flasher's poll for the trial (`_wait_for_trial()`). A request sent at
once after the `51 01` reaches no node: the node that answered is in its reset, and the trial is
not started yet. So the first `22 FD 00` goes 50 ms after it, when the restart through fcn_boot,
a few ms on the host (bench/updates.py measures it from the node's own log, `reset_to_trial`),
is normally over. Each request waits 50 ms, the node's P2server, within which a node that runs
answers it; one without an answer by then reached no node, or a node the host held up. The next
goes 50 ms after one that did not find the trial, up to `--trial-wait-s` (30 s). The step is
therefore the restart rounded up to the poll and one request (normally the first 50 ms and one
request), a time of the tester's and not the node's, and an update's total, the sum of the
steps, includes it. An answer that comes after its request's 50 ms is still sent, as the node
answers every request in order (RESPONSES in app/diag.h). One at most 100 ms late is dropped by
udsoncan before the next request; a later one is taken by the next request as its own, which is
the same `22 FD 00` with the same slot and mode, so no wrong answer. The answer to that next
request is then still due, and would reach read_version, whose `22 F1 89` it does not answer
(udsoncan refuses it: an unexpected response, a failed step); so after a request that went
unanswered, once the trial has answered, the flasher takes the answers still due, at most one
per unanswered request, each awaited 50 ms, before the step ends.

**One download, measured.** The release node of 244,368 bytes (stripped, with its header), 239
blocks, over the UDP bus: the report of `test_a_download_confirms_the_new_image[udp]` in the
release run of the system tests in one run of `scripts/check.sh` (its `report.json` in the
test's log directory, under `system-test/logs-functional/` of the build root; the times are the
tester's and include both hosts): erase 2.75 s (the node's log: 61 steps in 2,751,004 µs), transfer 19.70 s
(12,404 bytes/s; the median block 82.4 ms, the slowest 84.2 ms), check 3 ms, ecu_reset 0.7 ms,
wait_for_trial 1.20 s (the flasher of then: its first request at once, which nobody answers,
waited out P2client, 1 s, and the next came 0.2 s later: the tester's time, not the restart's,
which bench/updates.py measures as `reset_to_trial`), confirm 2.5 ms; over SocketCAN the erase
and the transfer took the same within 0.1 %, and the median block was the same to 0.1 ms (the
slowest 85.0 ms). The block time
is the diagnostic task's budget of 2 events per tick at work (the floor is 79 ticks). Phase 6's
benches measure it properly: [bench/isotp_throughput.py](../bench/isotp_throughput.py) the
transfer rate by block size from the tester to the node and the node's longest response to the
tester, [bench/updates.py](../bench/updates.py) 50
updates end to end and the download of a 256 KiB image (docs/measurements.md). Their final runs
at 500 kbit/s, host times like the ones above: blocks of 1024 bytes took 82.8 ms in the median
over SocketCAN and 82.9 ms over UDP (64 blocks each, above the floor of 79 ms), and the node's
longest response, the 71 bytes of `19 0A`, took 6.41 ms over SocketCAN and 6.43 ms over UDP on
the wire from the request to its last frame, in the median of 500
([results/isotp_throughput.json](../results/isotp_throughput.json)). All 50 updates over
SocketCAN and all 10 over UDP, of an image of 252,560 bytes in 247 blocks, were confirmed, each
by the tester's routine, in 24.5 s in the median (erase 2.85 and 2.84 s, transfer 20.4 and
20.5 s at 12,378 and 12,350 bytes/s, wait_for_trial 1.20 s), and all 10 downloads of a 256 KiB
payload over SocketCAN (257 blocks) were committed, the transfer 21.2 s at 12,375 bytes/s in
the median ([results/updates.json](../results/updates.json)).

**The bench.** `fcn_tools.node.Board`, started through `tests/conftest.py`'s
`start_board(*args, flash_file=, retained_file=, can=)`, starts the node through fcn_boot and,
after every requested reset (status 10) that the test announced with `Board.expect_resets(n)`,
starts the next run through fcn_boot with the same flash and retained cell, as a reset into the
bootloader starts a board again. An unannounced 10 stops the Board and fails the test; the end
checks of phases 3 and 4 apply to every run.

## Not implemented, and what a product adds

| Not in phase 5 | What a product does |
|---|---|
| SecurityAccess (0x27) | gates the programming session (and the confirm routine) behind seed and key, so that only an authorised tester can start a download; and the bootloader checks a signature over the image ([update.md](update.md#what-a-product-would-add)), which seed and key do not replace |
| Authentication (0x29) | certificate-based authentication of the tester |
| CommunicationControl (0x28) | the tester switches off the ECUs' normal communication (here, the telemetry) during programming, to give the download the bus |
| ControlDTCSetting (0x85) | the tester stops DTC setting during programming, so that the missing normal communication leaves no DTCs in the other ECUs |
| functional addressing (0x7DF) | requests to all ECUs at once (TesterPresent to keep every session, a session change before programming), with the standard's rules on which negative responses a server does not send to a functional request |
| WriteDataByIdentifier (0x2E) | the tester writes a fingerprint (who programmed, when) before the download |
| RequestUpload (0x35) | reads memory back; usually refused in production |
| compressed or encrypted downloads (dataFormatIdentifier other than 0x00) | a shorter transfer, or a confidential image the node decrypts as it writes |
| CAN FD | 64-byte frames, about eight times the data per frame |
| more than one DID per request; ReadDTCInformation's snapshot and extended data, 0x0B to 0x0E, the fault detection counter; DTC groups other than all; a MemorySelection | as the vehicle manufacturer's specification asks (lib/dtc has the read functions for 0x0B to 0x0E and the counter, [faults.md](faults.md)) |
| stopRoutine | aborts a running erase; here a routine runs to its end |
| ECUReset other than hardReset | keyOffOnReset, softReset, the rapid power shutdown |
| arbitration between testers | a server with several clients decides which request it serves, instead of answering every second one with 0x21 |

The services the node does not have are answered with serviceNotSupported (0x11).

## How it is tested

**Unit tests** (Unity, on a fake clock, deterministic; under ASan and UBSan and for coverage):
- [test_isotp.c](../tests/unit/test_isotp.c) (43 tests: every frame byte by byte, every check of
  received frames, every row of the unexpected-PDU table), [test_isotp_flow.c](../tests/unit/test_isotp_flow.c)
  (41: each timer at its deadline and 1 µs before, BS, STmin, WAIT at both sides, OVFLW,
  confirmations and the pipeline), [test_isotp_pair.c](../tests/unit/test_isotp_pair.c) (two
  executables: two instances over a simulated bus with an independent wire checker, every length
  from 0 to 4095 with five BS/STmin pairs, 600 random rounds with lost and repeated frames);
- [test_uds.c](../tests/unit/test_uds.c) (55: every service and sub-function, every NRC and the
  order of the checks, one test per step of the general response behaviour, SPRMIB, the
  responsePending cadence, S3server) and [test_uds_download.c](../tests/unit/test_uds_download.c)
  (30: the download on a fake backend, and again on lib/boot's real slot writer over lib/flash's
  NOR model with lib/bootimg, so that every refusal is made by the code that makes it on the node);
- [test_diag.c](../tests/unit/test_diag.c) (20: the node's connection, the confirmations matched
  by time, the response buffers, the ECUReset and its deadline) and [test_flash_job.c](../tests/unit/test_flash_job.c)
  (8: the jobs on the NOR model, the alive indications, the two confirms).

Coverage in a run of `scripts/coverage.sh` (part of `scripts/check.sh`): lib/isotp/isotp.c 525 of
525 lines and 365 of 365 branches, lib/uds/uds.c 540 of 542 lines and 270 of 275 branches (the two
lines not run are the unreachable default of `uds_session_bit()`), lib/uds/uds_program.c 231 of
231 and 154 of 154. Each module was challenged with planted defects in a scratch copy when it was
written: 27 in lib/isotp, 20 in lib/uds, 27 in the node's integration, the tester and the bench;
every one was caught.

**System tests** (pytest, the node as a process; functional unless marked `timing`):

| Test file | What it shows |
|---|---|
| [test_diag_isotp.py](../tests/system/test_diag_isotp.py) | frame by frame with python-can, on both transports: single frames padded to 8 bytes and unpadded ones understood; every kind of frame the node ignores (too short, a length the protocol does not allow, a reserved type, a stray CF or FC); the node's flow control `30 10 00` after the first frame and every 16th CF, never after the last, the SN wrapping, the longest request (1026 bytes) and OVFLW for one byte more and for the escape first frame; the tester's flow control kept (a block of BS 3, four WAITs then CTS, a fifth WAIT ending with N_WFT_OVRN, OVFLW and a reserved FS ending the transmission, no FC ending it after N_Bs), a request received while the node waits for an FC; a wrong SN, an SF and an FF in the middle of a reception, and a tester that stops (N_Cr), each followed by a request that is served. Each compares lib/isotp's counters with what the tester did, exactly. Timing: STmin 20 ms kept between the node's CFs |
| [test_diag_services.py](../tests/system/test_diag_services.py) | every request of a table of 84 in the default, extended and programming sessions without a flash, with exactly its answer: the general checks in their order (a service outside the session before any length; the sub-function and its session before the service's own length), SPRMIB suppressing positive answers and never negative ones, the session transitions, the services the node does not have, the DIDs without a hand-off, the routines' results; ReadDTCInformation's lists and counts by six masks against reportSupportedDTC; with a flash through fcn_boot, every check of the download's services and routines in its order, lib/boot's refusals behind them, an erase asked for with the SPRMIB, whose final positive answer comes all the same after its responsePending, the repeated block, transferDataSuspended ending the download, and a whole download committing the trial; ReadDTCInformation against BMS_Fault with a cell over its voltage limit, after it heals and after ClearDiagnosticInformation, whose answer comes only once the clear is made (0x19 straight after it finds the DTC confirmed no more). Every test compares the node's UDS counters (requests by service, NRCs by code, positive, suppressed, responsePending) with what the tester saw. Timing: responsePending at 40 ms and every 2 s during an erase of 110 sectors, TesterPresent within P2server; busyRepeatRequest for a TesterPresent and an unknown SID while an erase is pending (each answer is checked against the order the node answered in; that both requests came while it was pending is the host's); TesterPresent every second keeping the extended session past S3server |
| [test_download.py](../tests/system/test_download.py) | the flasher end to end on both transports: factory image, download, ECUReset, the trial started by fcn_boot, its version read and confirmed with the routine, every frame of the connection accounted for, and a restart that keeps it; the self-confirm when no tester confirms, after which the routine is refused and writes nothing; the slot writer refusing the running slot and a pending trial; a flash job that never returns ending in a watchdog reset; the DTC services read from the health task's snapshot. Timing: the block time; no frame lost under a flood of lower-priority frames (the bus model keeps the tester's frames only while the host reads the node's bus in time: a frame it dropped makes the run a missed tolerance) |
| [test_download_faults.py](../tests/system/test_download_faults.py) | an image damaged in transit and one for the other target, refused at the check routine (0x72), the old image started again, the next download committed; the crash payload downloaded, started on its three attempts and rolled back, the rollback in DID 0xFD00, in ReadDTCInformation (UPDATE_ROLLBACK) and in BMS_Fault, removed by ClearDiagnosticInformation; a download refused for both slots while a trial runs and accepted after the confirm routine; S3server in the middle of a download (abandoned, slot B WRITING, the next start runs slot A, the next download works); ECUReset with the SPRMIB in the middle of a download (no answer, the reset, slot A started again, the next download works); ECUReset from a tester that holds its earlier answer with WAITs and blocks of one frame (the node's frames in order, the reset, slot A started again through fcn_boot). Timing: that reset within 2 s and a margin of the request, the answer cut |
| [test_download_cuts.py](../tests/system/test_download_cuts.py) | 11 power cuts during a download, each a test of its own with its seed: the BEGIN record and its commit field (twice), the first and the last erase, the first, a drawn and the last page (the last one programmed by RequestTransferExit), the COMMIT record and its commit field; armed on the node's command line or at run time where no flash operation can come before the tester's next request (after RequestDownload's and RequestTransferExit's answers). After each: the run ended as a loss of supply in the step the operation belongs to, decision 5's invariants of phase 4 (update.md), the next start through fcn_boot as the state machine says (its frames and hand-off), and the next download, in that run, committed. The cut after COMMIT's last transition leaves a complete trial (the crash payload), which runs three times and is rolled back first |

The functional runs of these five files take about three minutes per build: their 34 tests took
174 s in the release run and 175 s in the asan run of `scripts/test-system.sh`, and their 7
timing tests 69 s in its timing run (the JUnit reports of one run of `scripts/check.sh` on the
development laptop). Planted defects in a scratch copy of
the node (one at a time, the release build rebuilt, the test that should see it run) were all
caught, 13 of them: 0x00 padding, BS 0 in the node's flow control, a fifth WAIT accepted, N_Cr
not started again by a consecutive frame, busy checked after the SID, the SPRMIB suppressing
negative answers, S3server keeping the download, a repeat taken before any block, the slot writer
erasing a sector short, a status mask that needs all its bits, the DTC snapshot published only at
the start, the SPRMIB's reset never made, and the validator built for the other target.
