# Fault handling

What the node does when something goes wrong: which faults it detects, how a fault becomes a
diagnostic trouble code (DTC) with the status byte a UDS tester reads, the fault frame that
carries the DTCs on the bus, the two watchdogs that turn a hang into a reset, and the
retained-RAM cell through which the next start learns why the last run ended. Phase 3 added all
of it; the CAN fault confinement, bus-off recovery and transmit buffering of phase 1
([can-simulation.md](can-simulation.md)) now feed the DTCs.

```
  sensor faults (sensor.*) ──> process task: a verdict per sample ──(verdict queue)──┐
  bus faults (can.*) ──> CAN controller model: error state, bus-off count ───────────┤
  sensor.hang ──> a task stops reporting that it is alive ──> watchdog manager ──────┤
  retained cell (--retained-file) ──> plat_reset_cause(): the cause of this start ───┤
                                                                                      v
                       health task, the one owner of the DTCs (lib/dtc) ──> BMS_Fault 0x010
                          │                     └──> summary ──> PackFaultActive, PackState FAULT
                          └──(kick, while the watchdog manager allows it)──> hardware watchdog
                                                                                │ not kicked
                                                                                v
                                    next start: reset cause WATCHDOG <── plat_reset(WATCHDOG)
```

The faults are applied from outside, as on a bench: sensor values and channel faults through
the `sensor.*` commands of simulation control, bus faults through `can.*`, a front end whose
read never returns through `sensor.hang`, a reset cause through the retained cell. The
application has no hook for them.

| Part | Code |
|---|---|
| DTCs and their status byte | [lib/dtc/dtc.h](../lib/dtc/dtc.h) |
| Watchdog manager (alive supervision) | [lib/wdgm/wdgm.h](../lib/wdgm/wdgm.h) |
| Hardware watchdog | [platform/include/plat_wdg.h](../platform/include/plat_wdg.h), host model [platform/posix/posix_wdg.c](../platform/posix/posix_wdg.c) |
| Retained-RAM cell and reset cause | [platform/include/plat_retained.h](../platform/include/plat_retained.h), host cell [platform/posix/posix_retained.c](../platform/posix/posix_retained.c) |
| The faults, their limits and debouncing | [app/fault.h](../app/fault.h), [app/fault.c](../app/fault.c) |
| Who does what, the supervision of the tasks | [app/app.c](../app/app.c) (IPC, SUPERVISION) |
| The fault frame | [dbc/fcn.dbc](../dbc/fcn.dbc), [app/telemetry.c](../app/telemetry.c), [can-protocol.md](can-protocol.md#0x010-bms_fault) |

## The faults and their DTCs

Every fault the node can detect is one event of lib/dtc, listed in one table in
[app/fault.c](../app/fault.c) with its DTC number, its debouncing and whether it requests the
warning indicator; each value is justified in a comment at its entry.

The DTC numbers are the project's own:

- the two high bytes are a code in SAE J2012 form, in the range a manufacturer defines: P1Axx
  for the battery module, U1xxx for the network, P1Bxx for the control module itself;
- the low byte is the failure type, 0x00 for every fault, so that each fault has a code of its
  own.

The fault number n is bit n of BMS_Fault's FaultActive and FaultConfirmed and entry n of the
value table of FaultMostRecent; app/telemetry.c checks every number against the header generated
from the DBC at compile time.

| n | Fault | DTC | Code | Set when | Debounce (set / heal) | Judged by |
|---|---|---|---|---|---|---|
| 0 | CELL_OVERVOLTAGE | 0x1A0100 | P1A01-00 | a valid cell reading > 4250 mV (released at ≤ 4200 mV) | counter ±5, one step per cell reading (every 20 ms): 5 readings from the start, 10 after a pass | process task, every sample with cells |
| 1 | CELL_UNDERVOLTAGE | 0x1A0200 | P1A02-00 | a valid cell reading < 2500 mV (released at ≥ 2600 mV) | ±5 cell readings | process task |
| 2 | OVERTEMPERATURE | 0x1A0300 | P1A03-00 | a valid temperature > 60.0 °C (released at ≤ 55.0 °C) | ±3 temperature readings (every 100 ms) | process task, every sample with temperatures |
| 3 | UNDERTEMPERATURE | 0x1A0400 | P1A04-00 | a valid temperature < −20.0 °C (released at ≥ −15.0 °C) | ±3 temperature readings | process task |
| 4 | OVERCURRENT_DISCHARGE | 0x1A0500 | P1A05-00 | a valid current > 250 A discharging (released at ≤ 225 A) | ±50 current readings (every 10 ms) | process task, every sample |
| 5 | OVERCURRENT_CHARGE | 0x1A0600 | P1A06-00 | a valid current > 100 A charging (released at ≤ 90 A) | ±50 current readings | process task |
| 6 | SENSOR_CHANNEL_INVALID | 0x1A1000 | P1A10-00 | a group the front end answered has a channel that is flagged invalid or implausible | the first evidence / the first sample with every channel valid | process task |
| 7 | FRONT_END_NO_ANSWER | 0x1A1100 | P1A11-00 | a read of the sample was not answered | the first evidence / the first sample answered in full | process task |
| 8 | SAMPLE_TIMEOUT | 0x1A1200 | P1A12-00 | the process task waited 30 ms (`APP_SAMPLE_TIMEOUT_MS`) for a sample in vain | the first timeout / the next sample | process task, every timeout |
| 9 | CAN_BUS_OFF | 0xD00100 | U1001-00 | a bus-off since the last health cycle, or bus off now | at once / 50 health cycles (5 s) without one | health task, every 100 ms |
| 10 | CAN_ERROR_PASSIVE | 0xD00200 | U1002-00 | an entry into error passive since the last health cycle, or not error active now | at once / 10 health cycles error active | health task |
| 11 | SUPERVISION_EXPIRED | 0x1B0100 | P1B01-00 | the watchdog manager's global status is EXPIRED or STOPPED | at once / never: EXPIRED is final and the node is reset | health task |
| 12 | RESET_WATCHDOG | 0x1B0200 | P1B02-00 | this start's reset cause (`plat_reset_cause()`) is the hardware watchdog | once, at the start of the run | `fault_init()`, before the scheduler starts |
| 13 | RESET_ASSERT | 0x1B0300 | P1B03-00 | the reset cause is a failed assertion | once, at the start | `fault_init()` |
| 14 | RESET_STACK_OVERFLOW | 0x1B0400 | P1B04-00 | the reset cause is a stack overflow | once, at the start | `fault_init()` |
| 15 | RESET_MALLOC_FAILED | 0x1B0500 | P1B05-00 | the reset cause is a failed allocation | once, at the start | `fault_init()` |
| 16 | UPDATE_ROLLBACK | 0x1B0600 | P1B06-00 | the bootloader's hand-off reports a rollback (ROLLBACK: a trial used its 3 attempts; REJECT: a trial did not validate; FALLBACK: the confirmed image did not validate) | once, at the start | `fault_init()`, from the hand-off (app/update.h) |

- **The limits** are those typical of NMC/graphite Li-ion cells of about 50 Ah, the cells the
  sensor model's load profile stands for ([sensor-model.md](sensor-model.md)): 4.20 V end of
  charge, 2.5 V cut-off, −20 to 60 °C in discharge, 5C peak discharge and 2C peak charge. A
  product takes them from the datasheet of its own cell. The model's values without a fault
  (cells 3359 to 3845 mV, temperatures 24.5 to 25.6 °C, current −80 to 160 A) lie inside every
  release value, so a run without a fault has no DTC. A reading between a limit and its release
  value is evidence neither way, so a value that sits at a limit with noise on it does not make
  the DTC come and go.
- **Evidence.** The limits are judged on the sample's raw readings, not on the filtered values
  (the filter would add its lag to the debouncing), and only from channels that are valid in
  the pack after `bms_process()`. A group the sample does not carry is not judged. A group the
  front end did not answer is FRONT_END_NO_ANSWER only, not also SENSOR_CHANNEL_INVALID. While
  no reading of a group is valid, its limits are not judged and their DTCs keep their state.
- **Debouncing.** The limits count symmetrically between −N and N without a jump down: N
  readings beyond the limit set the DTC from the start of a run, 2N net readings move it from
  one verdict to the other later, so that an excursion that is interrupted by good readings
  still adds up. A fault whose first piece of evidence decides (thresholds 1 and −1) is reported
  to lib/dtc as the matured FAILED or PASSED, because lib/dtc has no jump up. Bus off and error
  passive are set at once (the CAN error counters are their debouncing) and heal over 50 and 10
  health cycles with a jump down.
- Every DTC has a confirmation threshold of 1 (see [the operation cycle](#operation-cycle-clear-records)),
  so FaultConfirmed means "occurred since the start or the last clear".
- Faults 0 to 8, those of the pack's limits and its measurement, request the warning indicator.
- **UPDATE_ROLLBACK** (phase 4) is judged like the reset faults: the hand-off that the
  bootloader writes into the retained cell before it starts the image
  ([plat_retained.h](../platform/include/plat_retained.h), THE HAND-OFF) speaks to the start right
  after the rollback only, so the one report decides and the DTC stands for the run until a
  tester clears it. A start without the bootloader, or one that made no rollback, passes it. Its
  reason is in its entry of app/fault.c; [update.md](update.md) describes the rollback itself.
  It requests no warning indicator: it is a fault of the software, not of the module. **A
  stated limitation:** the report exists only in the start right after the rollback. A start
  that never gets as far as its first BMS_Fault (a loss of supply after the bootloader has
  written its rollback record and before the image runs, or a reset before the first health
  period) loses it, and the next start's hand-off has no rollback to report; the flash still
  shows it (the newest record's event in `fcn_flash info`), but the node does not read it from
  there. tests/system/test_update_cuts.py shows both sides: a cut that tears the rollback record
  leaves the next start to write it again and report it, one that completes it leaves nothing to
  report.
- A TESTER_Command stream that times out is no DTC: the tester is another node, and the node
  keeps the command it carried out last and goes on working. The state machine of the stream is
  in the statistics (`command.sm_state`).

## How a fault becomes a DTC: lib/dtc

[lib/dtc](../lib/dtc/dtc.h) turns the reports of the fault monitors into DTCs, each with the
status byte that a UDS tester reads. It is plain C11 with no state of its own: the application
owns one `dtc_state_t` and one constant table of events (`dtc_event_config_t`, the event id is
the index of its entry). The module calls no operating system, allocates nothing and does not
lock; its owner serialises all calls. The rules come from ISO 14229-1 Annex D (the definitions
of the statusOfDTC bits) and the AUTOSAR Diagnostic Event Manager (Dem, the counter-based
debouncing, cited by its parameter and type names). Where the norm leaves a choice, the header
marks it CHOICE, and the tables below repeat it.

`dtc_init()` refuses a table with a DTC number outside 0x000001..0xFFFFFE (an entry left out of
the table has 0x000000; 0xFFFFFF is the group with which ClearDiagnosticInformation means all
groups), a number used twice, or a parameter outside its range: failed threshold 1..32767,
passed threshold −32768..−1, steps 1..65535, confirmation threshold 1..255.

### Debouncing

A monitor reports one of the four values of `Dem_EventStatusType`. Each event has a counter
between its passed threshold (below 0) and its failed threshold (above 0):

| Report | Counter | Test result |
|---|---|---|
| PREFAILED | + step_up, capped at failed_threshold | FAILED when the cap is reached |
| PREPASSED | with jump_down and the counter above 0: first set to 0; then − step_down, capped at passed_threshold | PASSED when the cap is reached |
| FAILED | set to failed_threshold | FAILED |
| PASSED | set to passed_threshold | PASSED |

A pre-report that falls short of its threshold is evidence, not a test result: it moves the
counter and leaves the status byte alone. "The test completed" means that FAILED or PASSED was
reached. After a jump down the step of the same report is still applied (CHOICE). The start of
an operation cycle and a clear set the counter to 0 (CHOICE). The arithmetic is done in 32 bits,
so the largest step from an extreme threshold cannot overflow.

Example: failed threshold 5, passed −4, step up 2, step down 1. Three PREFAILED take the
counter to 2, 4 and 5, and the third is FAILED. Without jump down the DTC passes at the ninth
PREPASSED (4, 3, ..., −4); with jump down at the fourth (−1, −2, −3, −4), as it would from 0.
Until it passes, testFailed stays set.

The counter is also reported as the DTCFaultDetectionCounter of ISO 14229-1, −128..127, mapped
linearly and rounded toward zero (CHOICE), so only a counter at a threshold gives 127 or −128.

### The status byte

| Bit | Name | Set by | Reset by | Source and choice |
|---|---|---|---|---|
| 0 | testFailed (TF) | FAILED | PASSED; clear | Annex D. Kept at the start of an operation cycle, which performs no test |
| 1 | testFailedThisOperationCycle (TFTOC) | FAILED | start of a cycle; clear | Annex D |
| 2 | pendingDTC (PDTC) | FAILED | the end of a cycle in which the test completed and never failed (TNCTOC and TFTOC both 0); clear | Annex D. A PASSED alone does not reset it |
| 3 | confirmedDTC (CDTC) | FAILED, once `confirmation_threshold` operation cycles have had a failure | clear only (no aging) | ISO leaves the confirmation to the manufacturer; the number is the Dem's DemEventConfirmationThreshold. CHOICE: the cycles with a failure are counted since PDTC was last reset, so a completed cycle without a failure starts the count again and a cycle in which the test did not run does not; a cycle counts once however often it fails |
| 4 | testNotCompletedSinceLastClear (TNCSLC) | `dtc_init()`; clear | FAILED; PASSED | Annex D |
| 5 | testFailedSinceLastClear (TFSLC) | FAILED | clear only (no aging) | Annex D |
| 6 | testNotCompletedThisOperationCycle (TNCTOC) | start of a cycle (`dtc_init()` starts the first); clear | FAILED; PASSED | Annex D |
| 7 | warningIndicatorRequested (WIR) | together with CDTC, for an event with `warning_indicator` | clear only | ISO leaves the indicator to the manufacturer. CHOICE: requested from the confirmation until a clear; the Dem's healing of the indicator is not implemented |

After `dtc_init()` and after a clear every status byte is 0x50 (TNCSLC and TNCTOC). The module
implements all eight bits, so the DTCStatusAvailabilityMask is 0xFF. In the running node a DTC
that failed in this run and still fails has 0x2F (TF, TFTOC, PDTC, CDTC, TFSLC), 0xAF with the
warning indicator; once its test has passed again it has 0x2E or 0xAE.

### Operation cycle, clear, records

**Operation cycle.** One operation cycle covers all events: a run of the node from a reset.
`dtc_init()` begins the first; `dtc_start_operation_cycle()` ends a cycle (the pendingDTC rule)
and begins the next. In phase 3 the whole state is in RAM and a reset loses it, so every run
starts with every DTC at 0x50, is a single operation cycle, and the node never calls
`dtc_start_operation_cycle()`. A confirmation threshold above 1 could therefore never be
reached in the running node, which is why every DTC of app/fault.c has 1. Thresholds above 1,
and a pendingDTC that outlives a cycle, become meaningful only once the DTCs are kept in
non-volatile memory, which this project does not do (see the end of this page).

**Clear.** `dtc_clear_all()` is ClearDiagnosticInformation for all groups (0xFFFFFF): every DTC
goes back to its state after `dtc_init()`. A fault that is still present comes back as soon as
its monitor has debounced it again.

**Records.** Since the last clear the module keeps the first and the most recent DTC whose
testFailed rose and the first and the most recent whose confirmedDTC rose, what
ReadDTCInformation 0x0B, 0x0D, 0x0C and 0x0E report. A record changes on the rising edge of the
bit, not on every FAILED of a DTC that is still failed (CHOICE). BMS_Fault's FaultMostRecent is
the most recent testFailed record.

**Change count.** `dtc_status_change_count()` counts status-byte changes (not changes of the
debounce counter alone); the statistics give it as `faults.status_changes`.

### Read access over UDS (phase 5)

A tester reads and clears the DTCs over the bus with the node's UDS server
([diagnostics.md](diagnostics.md)), in the default and the extended session:

| UDS request | Answer | lib/dtc |
|---|---|---|
| ReadDTCInformation `19 01 <mask>`, reportNumberOfDTCByStatusMask | `59 01 FF 01 <count, 2 bytes>`: the DTCStatusAvailabilityMask 0xFF, the DTCFormatIdentifier 0x01 (ISO_14229-1_DTCFormat) | `dtc_count_by_status_mask()`, `DTC_STATUS_AVAILABILITY_MASK` |
| ReadDTCInformation `19 02 <mask>`, reportDTCByStatusMask | `59 02 FF` and, for every DTC that matches, its 3-byte number and its status, in table order | `dtc_next_by_status_mask()` with a cursor |
| ReadDTCInformation `19 0A`, reportSupportedDTC | `59 0A FF` and every DTC of the table with its status, 0x00 included (71 bytes for the 17 DTCs) | `dtc_get()` |
| ClearDiagnosticInformation `14 FF FF FF` | `54`, once the health task has made the clear | `dtc_clear_all()`, in the health task |

A DTC matches a status mask if (statusOfDTC & mask) ≠ 0, so a mask of 0 matches none. The
answers are built from a snapshot of lib/dtc's state that the health task, the DTCs' only owner,
publishes at the end of every period, in a critical section, with the count of clears it has
carried out; a read therefore describes the end of the health task's last period, the same
picture as that period's BMS_Fault (the system tests compare the two DTC by DTC), and is at most
one period and the health task's response time old. ClearDiagnosticInformation is a clear request
of its own, added to those of TESTER_Command, which the health task carries out as below
(Clearing) and then reports to the diagnostic task: the positive answer never comes before the
clear. Not served (lib/dtc has the functions, lib/uds does not offer them): ReadDTCInformation
0x0B to 0x0E (the first and most recent test-failed and confirmed DTC, `dtc_recorded()`), 0x14
reportDTCFaultDetectionCounter (`dtc_get()`, `fault_detection_counter`), snapshot and extended
data, groups other than all, a MemorySelection.

## Who owns the DTCs

The DTC state (`fault_monitor_t`, [app/fault.h](../app/fault.h)) has one owner, the health task.
`fault_init()` runs once in `app_start()`, before the scheduler starts; every other call on it is
made in the health task.

- **The processing task** judges each sample, or each timeout of its wait for one, with
  functions that keep no state (`fault_judge_sample()`, `fault_judge_timeout()`). It posts the
  verdict, two 16-bit masks of failing and passing evidence and the count of clear requests
  it read as it posted it (8 bytes), to a queue of 32 (`APP_FAULT_VERDICT_QUEUE_LENGTH`) that
  it never waits on; a lost verdict is counted (`process.verdicts_lost`). It writes no log
  line: it counts its timeouts and the samples that come after one (`process.timeouts`,
  `process.recoveries`), and the health task writes the lines.
- **The health task**, in every 100 ms period:
  1. runs one supervision cycle of the watchdog manager and kicks the hardware watchdog if the
     manager allows it;
  2. logs a stall of the samples that began (`no sample from the sensor task for 30 ms`) or
     ended (`samples again after <n> timeouts`) since its last period, from the processing
     task's counts;
  3. reads the CAN controller (the bus-off count of the transmit queue, the count of entries
     into error passive, the controller's state) and the watchdog manager's global status,
     then the count of clear requests;
  4. works in up to 32 verdicts, in the order of the samples, so that the debouncing counts
     samples, and clears the DTCs before the first verdict that carries a clear request it has
     not carried out (see Clearing);
  5. carries out a clear request that no verdict carried;
  6. judges the CAN controller and the watchdog manager from what it read in step 3;
  7. logs every change of a DTC's testFailed in the period, and publishes the summary
     (`fault_summary_t`) in a critical section;
  8. sends BMS_Fault.
- **The receive task** counts the clear requests of TESTER_Command (`command_rx_t.clear_requests`),
  and the diagnostic task those of ClearDiagnosticInformation (phase 5); the health task reads
  their sum, so a clear by UDS is carried out like one by TESTER_Command. After its period the
  health task publishes a snapshot of the DTCs for the diagnostic task's ReadDTCInformation
  (above) and notifies that task after a clear.
- **The processing task** reads the summary in a critical section for every sample:
  BMS_PackStatus says PackFaultActive and PackState FAULT while a fault is active in the
  summary of the health task's last period.

The DTCs change only in the health task's job, so BMS_Fault, sent at the end of that job, goes
out at once on every change, and every 100 ms without one. The cost is latency: a fault of the
pack values reaches its DTC in the first health period after the sample that decided it, at
most 100 ms later.

**The summary of a period.** A job works in up to 100 ms of evidence, and a fault that one
piece of evidence decides (SENSOR_CHANNEL_INVALID, FRONT_END_NO_ANSWER, SAMPLE_TIMEOUT) can
fail and pass again within it. So the fault monitor keeps the rises and falls of each DTC's
testFailed between two summaries, and the summary has a fault active if its testFailed is set
at the end of the period or rose during it: such a fault is active in BMS_Fault for one frame
and in PackFaultActive for the period after it, and the log has both its lines. A fault that
rose and fell gets its two lines in that order, one that fell and rose again in the other; one
that changed more often in one period gets those two lines all the same. A clear forgets the
rises before it, so that the frame of a clear holds nothing from before the request, and keeps
the rises after it.

The processing task, the task that sees the samples, is not the owner because it runs above
`can_tx`: every microsecond in its budget is one more in `can_tx`'s response time. With the
owner's work in the processing task (its budget 160 + 54 µs: the reports, the summary and
BMS_Fault) the schedulability check finds `can_tx` missing its deadline, R = 1209 µs against
1000 µs ([architecture.md](architecture.md#schedulability)).

## The fault frame: BMS_Fault (0x010)

BMS_Fault has the most urgent identifier on the bus. It is 8 bytes, protected with AUTOSAR E2E
Profile 11A under Data ID 0x0110 (the CRC of the Data ID's bytes is 0xA9, distinct from every
other protected message's), sent every 100 ms and whatever CmdTelemetryOn says, from transmit-queue
slot 7.

| Signal | Bits | Meaning |
|---|---|---|
| E2E_Crc, E2E_Counter | 0..7, 8..11 | Profile 11A |
| FaultActive | 16..32 | bit n: the DTC of fault n has testFailed at the end of the period, or its testFailed rose during it (after the clear, in the period of a clear) |
| FaultConfirmed | 33..49 | bit n: the DTC of fault n has confirmedDTC: the fault occurred since the start or the last clear |
| FaultMostRecent | 50..54 | the fault whose testFailed rose most recently since the start or the last clear; 31 NONE |
| FaultWarning | 55 | a DTC requests the warning indicator |
| FaultClearCount | 56..63 | clears carried out since the start, modulo 256 |

[can-protocol.md](can-protocol.md#0x010-bms_fault) has the full signal table.

**Clearing.** A tester clears the DTCs by changing CmdClearFaultsSeq between two commands the
node carries out while the command stream's E2E state machine stays VALID (see
[can-protocol.md](can-protocol.md#the-nodes-receiver-of-tester_command)). The first command
carried out after the stream started, or after it was lost, only sets the reference, so a
tester that starts, another tester after one went away, or a tester or pack master that comes
back after a timeout or its own reset clears nothing, whatever value it brings, as long as its
stream was lost in between. The node can tell only that: a sender whose stream is not lost,
one that is silent for at most one period (two, if its E2E counter happens to go on in
sequence; [can-protocol.md](can-protocol.md#the-nodes-receiver-of-tester_command)), is taken
for the one before it, so a pack master that resets that quickly and comes back with another
value requests a clear. The price of the rule is that a tester that changed its value while its
stream was lost has requested nothing: it sees FaultClearCount unchanged and changes the value
again.

The receive task counts a request when the command arrives; the health task carries it out in
order with the evidence. Each verdict carries the count of requests the processing task had
read when it posted it, and the health task clears every DTC before the first verdict that
carries a count it has not carried out, or after the period's verdicts for a request that no
verdict carried, and before it judges the CAN controller and the watchdog manager, whose
inputs it read before the count. So a clear erases the evidence handed over before its request
and none handed over after it: the frame of the clear has no DTC active or confirmed, no most
recent one and no warning indicator from before the request, and a fault detected after the
request is in it already or, handed over after the clear's period, in the frame of the period
it was handed over in. (Evidence of the samples is "handed over"
when the processing task reads the count for its verdict, just before it posts it. The CAN
controller's inputs say what happened since the health task's last period, and the job cannot
tell what of it came before the request: a bus-off or an entry into error passive between the
last period and the request survives the clear.)
Requests that arrive between two verdicts are carried out once, however many they are. A fault
that is still present comes back as soon as it has been debounced again. The reset faults are
judged only at the start of a run, so once cleared they stay cleared for the rest of it.

**What the node does about a fault in phase 3: it reports it.** It sets the DTC, sends BMS_Fault,
sets PackFaultActive and PackState FAULT, and writes a line to its log for every change of the
DTC's testFailed, a clear included
(`fault CELL_OVERVOLTAGE active (DTC 0x1A0100)`, `fault CELL_OVERVOLTAGE no longer active`). It
has no contactor or current limit to command; opening the contactors is the decision of the
pack's master, which reads BMS_Fault.

**A start after a reset caused by a fault is a fault of its own.** The matching RESET_* DTC is
active and confirmed from the first BMS_PackStatus and the first BMS_Fault of the run on (it is
judged before the scheduler starts) and stays active until a clear, so that the pack's master
learns of it. BMS_NodeInfo.ResetCause carries the cause of every start: 0 POWER_ON, 10 to 22 the
reset reasons of `plat_reset_reason_t`.

## The watchdogs

A node that hangs is worse than a node that resets: it keeps its place on the bus while nothing
it says is current. Two watchdogs stand between a hang and that state, one in software and one
in (simulated) hardware.

- The **watchdog manager** finds a task that stops while the others run on: every supervised
  task reports that it is alive, and the supervising task checks the reports at a fixed period.
- The **hardware watchdog** finds everything else: a supervising task that stops, a task that
  keeps the CPU so that the supervising task never runs, a scheduler that stopped. Only the
  supervising task kicks it, and only while the watchdog manager allows a kick.

### The watchdog manager (lib/wdgm)

[lib/wdgm/wdgm.h](../lib/wdgm/wdgm.h) implements the Alive Supervision of the AUTOSAR
"Specification of Watchdog Manager" (AUTOSAR_SWS_WatchdogManager, CP R22-11, sections 7.1.2,
7.1.3 and 7.2.1) in pure C, with the state in caller memory. The requirement numbers are quoted
in the code where each rule is implemented.

**Supervised entities.** The caller describes each entity in a table; its index is its
identifier. Each entity has one checkpoint, the simplest configuration of the SWS: a task that
calls `wdgm_checkpoint()` once per loop is one entity.

| Parameter | SWS configuration parameter | Meaning |
|---|---|---|
| `expected_alive` | WdgMExpectedAliveIndications | indications expected per reference cycle, at least 1 |
| `min_margin` | WdgMMinMargin | fewer indications that are still correct; less than `expected_alive` |
| `max_margin` | WdgMMaxMargin | more indications that are still correct |
| `reference_cycle` | WdgMSupervisionReferenceCycle | supervision cycles per examination, at least 1 |
| `failed_tolerance` | WdgMFailedAliveSupervisionRefCycleTol | failed reference cycles tolerated |
| `expired_tolerance` (`wdgm_config_t`) | WdgMExpiredSupervisionCycleTol | supervision cycles in EXPIRED, still kicking, before STOPPED |

**Examination.** The supervising task calls `wdgm_main_function()` once per supervision cycle.
Every `reference_cycle`-th call examines each entity's count and starts it again from 0; the
count is correct when `expected_alive − min_margin ≤ count ≤ expected_alive + max_margin`
(SWS_WdgM_00074, SWS_WdgM_00115).

**Local Supervision Status** after each examination (F is the failed reference cycle counter,
T the `failed_tolerance`):

| From | Result | Condition | To | F | Requirement |
|---|---|---|---|---|---|
| OK | correct | | OK | | SWS_WdgM_00201 |
| OK | incorrect | T = 0 | EXPIRED | | SWS_WdgM_00202 |
| OK | incorrect | T > 0 | FAILED | 1 | SWS_WdgM_00203 |
| FAILED | incorrect | F < T | FAILED | F + 1 | SWS_WdgM_00204 |
| FAILED | incorrect | F = T | EXPIRED | | SWS_WdgM_00206 |
| FAILED | correct | F > 1 | FAILED | F − 1 | SWS_WdgM_00300 |
| FAILED | correct | F = 1 | OK | 0 | SWS_WdgM_00205 |
| EXPIRED | any | | EXPIRED | | cannot be left |

An entity is allowed T failed reference cycles, net of the good ones in between; the (T + 1)-th
makes it EXPIRED. An entity that stops reporting altogether is EXPIRED within T + 2 reference
cycles of its last indication.

**Global Supervision Status**, in every call (E is the expired cycle counter, X the
`expired_tolerance`): OK while every entity is OK; FAILED while one is FAILED and none EXPIRED;
when an entity expires, EXPIRED with E = 1 if X > 0 or STOPPED at once if X = 0; in EXPIRED, E
counts every supervision cycle up to X, then STOPPED, which is final.

**The kick.** `wdgm_may_kick()` is true in OK, FAILED and EXPIRED and false in STOPPED
(SWS_WdgM_00223, SWS_WdgM_00122). `wdgm_first_expired()` names the entity that expired first
(WdgM_GetFirstExpiredSEID, SWS_WdgM_00346). Before `wdgm_init()` the status is DEACTIVATED, and
there the module differs from the SWS on purpose: the SWS leaves the trigger condition alone in
DEACTIVATED, but a manager that was never initialised must not keep a started hardware watchdog
alive, so `wdgm_may_kick()` is false. The worst case from a supervised task's last alive
indication to the reset is (T + 2) reference cycles, plus X supervision cycles, plus the
hardware watchdog's timeout.

**Stricter than the SWS.** A lower bound of 0 (`min_margin ≥ expected_alive`) is refused: it
would accept an entity that never runs again. `wdgm_main_function()` checks the configuration
and the state before it uses them; a configuration that is not valid or not the size given to
`wdgm_init()`, or a state that no sequence of calls can produce, makes the global status
STOPPED, so that a manager that cannot trust its own data lets the hardware watchdog reset the
node.

**Concurrency.** `wdgm_checkpoint()` may be called from any number of tasks and interrupt
handlers at once, and at the same time as `wdgm_main_function()`: it is one relaxed atomic
fetch-and-add on the entity's `_Atomic uint32_t`, and the examination takes the count with one
atomic exchange for 0, so every indication is counted by exactly one examination and the caller
provides no critical section. The build needs lock-free 32-bit atomics, which a
`_Static_assert` checks (a Cortex-M3 or M4 has them; a Cortex-M0 does not). Everything else of
the manager belongs to the supervising task. `<stdatomic.h>` is a C11 feature, which MISRA C:2012
rule 1.4 covers; the project's documented deviation of that rule
([misra-deviations.md](misra-deviations.md)) names the manager's counters.

**Left out.** Deadline and logical supervision; modes and the activation of entities; several
checkpoints per entity; WdgM_PerformReset; reporting to a Dem (the application records the
failure as a DTC); several main function instances; the watchdog interface (the caller kicks).

### The hardware watchdog (plat_wdg.h)

[platform/include/plat_wdg.h](../platform/include/plat_wdg.h) is an independent watchdog, as the
IWDG of an STM32 is:

- `plat_wdg_start(timeout_ms)` starts it with a timeout of 1 to 26214 ms (`PLAT_WDG_TIMEOUT_MIN_MS`,
  `PLAT_WDG_TIMEOUT_MAX_MS`: the longest timeout of an STM32F1 IWDG, 4096 counts of its 12-bit
  reload register at the largest prescaler, 256, on the nominal 40 kHz of its LSI clock:
  4096 × 256 / 40000 Hz = 26.2144 s). The start counts as the first kick. A timeout out of range
  and a second start are refused: once it runs, the watchdog cannot be stopped or reconfigured.
- `plat_wdg_kick()` starts the count again. On a target it is one store to the key register
  (0xAAAA to IWDG_KR). On the host it is one read of CLOCK_MONOTONIC and two atomic operations,
  with no lock and nothing that blocks.

**The host model** ([platform/posix/posix_wdg.c](../platform/posix/posix_wdg.c)) is a thread
outside the scheduler, created by `plat_wdg_start()`, that stands for the watchdog's own clock.
Every millisecond it adds the time since its last pass to the time counted since the last kick;
when that reaches the timeout it writes `FATAL watchdog: not kicked for <n> ms (timeout <t> ms)`
to stderr and calls `plat_reset(PLAT_RESET_WATCHDOG)`: the process ends with exit status 11.

- **Never early**: the counted time is never more than the real time since the last kick it saw.
- **Late by at most one pass and the host's wake-up latency** while the host runs the thread.
- **Host stalls**: a gap of more than 50 ms between two passes counts as 50 ms and is counted in
  `hw_watchdog.gaps`. A host that stops running the process stops the node's tasks as well, as a
  target's watchdog does not count while its chip is not clocked, so such gaps can only delay a
  reset, never cause one.
- A kick that races the expiry is lost, and the reset happens: that kick came after a whole
  timeout without one.
- The orderly end of a run, which exists only on the host (SIGTERM, `--duration-s`), switches
  the simulated board off: `plat_end_scheduler()` stops the model.

`plat_reset()` may be called from that thread: it enters a FreeRTOS critical section only from a
task's thread, and when two contexts reset at once, the first one's reason is recorded.

### The watchdogs in the node

**Supervised tasks** ([app/app.c](../app/app.c), SUPERVISION). The sensor, process, can_rx and
can_tx tasks each report an alive indication once per pass of their loop (the CAN tasks through
`can_io_on_alive()`). The health task runs one supervision cycle per period (reference cycle 1,
100 ms) and then kicks, if `wdgm_may_kick()` allows it.

| Entity | Expected per 100 ms | Correct count | Why |
|---|---|---|---|
| sensor | 10 | 1 to 20 | a pass every 10 ms; at most twice that, so that a loop whose wait stops waiting is found too |
| process | 10 | 1 to 21 | a pass per sample, and per 30 ms timeout while the samples do not come |
| can_rx, can_tx | 2 | 1 to 65535 | a pass per wake-up, at least every 50 ms, up to one per frame on the bus: a lower bound only, in effect |
| diag (phase 5) | 2 | 1 to 65535 | the same, for a pass per event or deadline, at least every 50 ms |
| flash (phase 5) | 2 per 600 ms | 1 to 65535, examined every 6 cycles (reference cycle 6), failed tolerance 1 | a pass per job or request and at least every 100 ms between them, and an indication after every call of lib/boot in a job; one call can wait for the part 409 ms at its maximum times, so a window of 600 ms (no less than 555 ms with the health task's response time) is the shortest that a working flash task always reaches ([app/app.c](../app/app.c), SUPERVISION) |

The lower bound of every entity is 1: the supervision is there to find a task that has stopped.
Timing within a period is the business of the overrun counters (BMS_TaskStats). A task that
goes on after a stall stays below the upper bounds. The sensor task does not make up the
periods it missed after an overrun: it skips them, keeps the grid of its 10 ms period, and
counts them (`sensor.skipped_periods`; BMS_TaskStats counts them as overruns), so between two
examinations it makes a pass for every period of the grid that begins in between and, after a
stall, at most two more (the end of the pass that stalled, and at once the pass of the period
the tick is in): 12 when the examinations are 100 ms apart, 17 when the health task's response
time, under 45 ms in the analysis of [architecture.md](architecture.md#schedulability) since
phase 5's diagnostic task (under 20 ms before it), puts them 145 ms apart. The heartbeat's
PeriodCounter and the sample numbers count its passes, so they go on without a gap. The
processing task, after a stall of its own, finds at most the 4 samples the queue held and then
one per pass of the sensor task, at most 21 in a cycle (its upper bound was 20 before phase 5);
it times out only while no sample comes. The failed tolerance is 2, so that one pass of an
event-driven task that slips past the end of a cycle is not taken for a hang, and a task that
stands still for up to two cycles and then goes on is not reset for it: the cycle in which it
goes on is correct again, and the good cycles after it make the entity OK. Measured from the
task's last alive indication before the stall to its first after it, a stall of less than
300 ms leaves at most two examinations without an indication, wherever it falls against the
health period, and one of more than 400 ms leaves three, which make it EXPIRED; the health
task's response time moves an examination, and these figures, by up to its worst case. The
expired tolerance is 0. The health task is not supervised: it is the task that
kicks, so the hardware watchdog covers it. Neither is the idle task: it does nothing that could
hang, and a task that keeps the CPU starves the health task as well. The hardware watchdog's
timeout is `APP_WDG_TIMEOUT_MS`, 500 ms, five health periods, so that a late health job does not
reset the node; the build checks it against the IWDG's range.

**What happens when a task stops** (h the moment it stops, a its last alive indication, 0 to
10 ms before h for the sensor task; j1 the first health job after a, 0 to 100 ms later, which
still counts it):

1. j1 + 100 ms and j1 + 200 ms: two examinations without an indication; the entity is FAILED.
2. j1 + 300 ms, 300 to 400 ms after a: the third makes it EXPIRED and the global status STOPPED.
   That health job logs `supervision: <task> expired (<n> alive in its last cycle); no more
   kicks: reset within 500 ms`, sets SUPERVISION_EXPIRED, which goes out in that period's
   BMS_Fault, and withholds its kick.
3. j1 + 700 ms, 500 ms after the last kick: the hardware watchdog resets the node, 400 ms after
   the frame that reported the expiry and 700 to 800 ms after the last indication. The exit
   status is 11 and the retained cell records WATCHDOG.

A task that goes on again before step 2 is not reset: its next indication makes the cycle
correct, and SAMPLE_TIMEOUT, active while the sensor task's samples did not come, heals with
the first sample. One that goes on after step 2 is reset all the same: STOPPED is final.

A task that keeps the CPU (`sensor.hang spin`) starves every task below it, the health task with
its kicks and its BMS_Fault: nothing in the node can record the fault, and the hardware watchdog
resets the node 500 ms after the last kick, 400 to 510 ms after the task started spinning. The
next start reports RESET_WATCHDOG either way.

## The retained-RAM cell (plat_retained.h)

A microcontroller keeps its RAM through a reset, so a small struct in a section the start-up code
does not zero still holds after a reset what was written into it before; after power-on it holds
garbage. [platform/include/plat_retained.h](../platform/include/plat_retained.h) defines that
struct, and `plat_reset_cause()` the reset cause of this start, read from it.

| Offset | Size | Member | Meaning |
|---|---|---|---|
| 0 | 4 | `magic` | `PLAT_RETAINED_MAGIC`, the bytes `FCNR` |
| 4 | 2 | `version` | `PLAT_RETAINED_VERSION`, 1 |
| 6 | 2 | `size` | 36, the size of the cell |
| 8 | 4 | `reset_reason` | what the next start reports: a `plat_reset_reason_t` |
| 12 | 4 | `resets` | `plat_reset()` calls since power-on |
| 16 | 16 | `reserved[4]` | the bootloader's hand-off to the image (phase 4): the slot, trial or confirmed, the attempt, the image version, the state change it made (plat_retained.h, THE HAND-OFF); 0 when there is none |
| 32 | 4 | `crc` | CRC-32/ISO-HDLC (lib/crc, equal to `zlib.crc32`) over bytes 0 to 31 |

Native byte order, little-endian on every machine the project builds for, no padding
(`_Static_assert` on the size and the offsets).

**At start**, before the application runs, the platform reads the cell once:

| The cell | Cause of this start | Verdict (`reset.verdict`) | Then |
|---|---|---|---|
| no `--retained-file` | `PLAT_RESET_POWER_ON` | `no cell` | nothing is retained |
| the file did not exist | `PLAT_RESET_POWER_ON` | `missing` | created, written afresh |
| the file is not 36 bytes | `PLAT_RESET_POWER_ON` | `size` | resized, written afresh |
| bad magic | `PLAT_RESET_POWER_ON` | `magic` | written afresh |
| bad version or size member | `PLAT_RESET_POWER_ON` | `version` | written afresh |
| bad CRC | `PLAT_RESET_POWER_ON` | `crc` | written afresh |
| a reason `plat_reset()` does not record (POWER_LOSS, any other value) | `PLAT_RESET_POWER_ON` | `reason` | written afresh |
| reason `PLAT_RESET_POWER_ON` | `PLAT_RESET_POWER_ON` | `no reset` | written afresh |
| reason REQUESTED, WATCHDOG, IMAGE_FAULT, ASSERT, STACK_OVERFLOW or MALLOC_FAILED | that reason | `reset` | the reason is set back to POWER_ON; `resets` and the reserved words are kept (but for a hand-off, below) |

**The hand-off (phase 4).** In every cell whose magic, version, size and CRC are right, whatever
its reason, the platform first reads the reserved words as the bootloader's hand-off: if
`reserved[0]` is `PLAT_HANDOFF_MAGIC` (`FCNH`) and the other words keep the rules of
plat_retained.h, `plat_boot_handoff()` returns it for the run (the statistics' `handoff.verdict`
is `taken`; `invalid` for one that breaks a rule, `none` without one), and the four words are
zeroed, so that a later start without the bootloader finds none. The bootloader (fcn_boot) opens
the cell before the image does with `posix_retained_open_boot()`, which leaves the recorded
reason in it for the image (and zeroes a hand-off an earlier start left), and writes the
hand-off with `posix_retained_set_handoff()` directly before it starts the image. It then
releases its lock of the cell (`posix_retained_unlock()`) for the image, which locks it itself;
if the start fails, it takes the lock back (`posix_retained_relock()`) and
`posix_retained_clear_handoff()` takes the hand-off back (update.md, the hand-off, which also
states the window on the host in which a hand-off can stay behind or reach another program). A
cell that is not valid it writes afresh first, as after power-on, so the image of a start after
power-on finds a valid cell with no reset and its hand-off (verdict `no reset`).

"Written afresh" means every word 0 (reason POWER_ON) with a valid header and CRC. Because the
start spends the reason, a run that ends without `plat_reset()` (on the host an orderly stop or a
killed process: the bench switched the board off) leads to a power-on at the next start. **At a
reset**, `plat_reset()` writes its reason, counts the reset and seals the cell with a new CRC
before the process exits; `plat_reset(PLAT_RESET_POWER_LOSS)` zeroes the cell instead, as a loss
of supply would. A value that is no reset reason (`PLAT_RESET_POWER_ON` among them) is a
programming error and resets as `PLAT_RESET_ASSERT`. On the host its last step before the exit
is to release the locks of the cell's file and of the flash file (below).

**On the host** ([platform/posix/posix_retained.c](../platform/posix/posix_retained.c)) the cell is
the file named with `--retained-file PATH`, mapped with `mmap(MAP_SHARED)`: every store lands in
the page cache at once and is seen by the next process that opens the file, however this one
ends. A process killed in the middle of the stores leaves a cell whose CRC does not match, which
the next start takes as a power-on. The bench removes the file to cut the power. The file is
locked with `flock()` while a node runs; a second node that names it fails to start
(`--retained-file PATH is in use by another node`, exit status 1), and so does a path that cannot
be created (`cannot write --retained-file PATH`). Without the option every start is a power-on.

The node releases that lock itself at every end it controls, and the flash file's with it: by
the close at an orderly stop, and with `flock(LOCK_UN)` as the last step before `_exit()` at
every reset (`plat_reset()`, whatever the reason and whichever thread resets) and in the
shutdown guard, after the reset has been recorded: once the lock is released nothing writes
into the cell. A lock left to the exit would last as long as the process's memory map, because
the cell's mapping refers to the locked open file description, and another process can hold
that map for a moment after the exit; the next start, which a bench makes as soon as it has seen
the exit, then failed with "in use" ([bug 012](bug-log.md#012-the-next-start-found-the-files-still-locked-by-the-run-that-had-just-ended)).
A killed node (SIGKILL) releases nothing: its lock goes when the kernel has taken its memory map
down.

The node reports what it found in its log (`reset cause WATCHDOG (retained cell: reset, 1 resets
since power-on)`) and in its statistics (`reset`: `cause`, `cause_code`, `retained_cell`,
`verdict`, `resets_since_power_on`).

## The hang of the sensor front end (sensor.hang)

The watchdog tests need a task that hangs without a test hook in the application: the
simulation control command `sensor.hang on|spin|off` makes every read of the front end wait until
`off` (or `sensor.clear`), the hardware fault of a transfer that never completes
([sensor-model.md](sensor-model.md)).

- `on`: the read waits as a driver waits for its transfer-complete interrupt. The sensor task is
  blocked (one tick of `vTaskDelay` at a time), the tasks below it run on, the supervising task
  included, and only what watches the sensor task itself sees that it stopped: the watchdog
  manager (its alive count stops) and the processing task's timeout on the samples that no
  longer come (SAMPLE_TIMEOUT). This is the path through the watchdog manager.
- `spin`: the read waits as a driver polling a busy flag. The sensor task never gives up the CPU,
  every task below it starves, the supervising task with them, and only the hardware watchdog
  can see it.

`sensor.status` reports `hang=off|on|spin` and `hung=<n>`, the reads that had to wait.

## What a target would use

- **The watchdog: the IWDG.** Start with 0xCCCC to IWDG_KR; unlock the prescaler and reload
  registers with 0x5555 and write PR and RLR (waiting for IWDG_SR to clear); kick with 0xAAAA.
  Its LSI clock varies widely from part to part, so the timeout is set for the fastest LSI the
  datasheet allows. The option byte WDG_SW can select the "hardware watchdog", which the chip
  starts by itself at every reset, so that a hang before the application starts it is caught
  too. DBGMCU_CR.DBG_IWDG_STOP stops it while a debugger halts the core. The window watchdog
  (WWDG) would add a lower bound on the kick period; the host model has none.
- **The reset cause: the RCC_CSR flags.** PORRSTF (power-on or power-down), PINRSTF (the NRST
  pin), SFTRSTF (a software reset, SYSRESETREQ), IWDGRSTF, WWDGRSTF and LPWRRSTF. The start-up
  code reads them before anything else and clears them with RMVF. IWDGRSTF gives
  `PLAT_RESET_WATCHDOG`: the one difference to the host model, because no code runs before a
  watchdog reset to write the reason into the cell (on the host the model's thread calls
  `plat_reset()` like any other reset). SFTRSTF sends the start to the cell for the software
  reasons. PORRSTF means power-on, whatever the cell holds. A pin reset has no value in
  `plat_reset_reason_t` yet.
- **The retained cell: a `.noinit` section.** `plat_retained_cell_t` placed with
  `__attribute__((section(".noinit")))` in an output section of type NOLOAD outside `.bss` and
  `.data`, so that neither the start-up code's zeroing nor its copying from flash touches it. The
  same magic, version and CRC tell a cell from the garbage of a power-on. The bootloader shares
  the cell with the application (the hand-off, phase 4), so both linker scripts need it at the
  same address (phase 7).
- **The DTCs** are read and cleared over UDS (phase 5) as they are; keeping them across a
  reset would need a function that restores a saved state, which this project does not add.

## Measured on the development laptop

Host timing only, not a property of a target: WSL2 Ubuntu 24.04, release build, load average 0.3, 2026-11-20. The
system tests print these figures:

```bash
scripts/build.sh release
PYTHONPATH=tools python -m pytest tests/system/test_faults.py -m timing -rP
```

Each figure is from the command's monotonic time stamp (`t_ns` in the simulation control
reply) to the moment the tester read the frame, or saw the process end (a thread blocked in
`waitid()`). UDP / SocketCAN where both ran:

| What | Measured | Bound the test applies |
|---|---|---|
| `sensor.hang on` to SAMPLE_TIMEOUT in BMS_Fault | 99.4 / 98.8 ms | 20 to 160 ms |
| `sensor.hang on` to SUPERVISION_EXPIRED in BMS_Fault | 298.7 / 298.4 ms | 290 to 430 ms |
| `sensor.hang on` to the end of the process (status 11) | 699.8 / 699.2 ms | 690 to 850 ms |
| the frame reporting the expiry to the end of the process | 401.0 / 400.9 ms | 390 to 450 ms |
| `sensor.hang spin` to the end of the process | 502.2 / 499.7 ms | 400 to 560 ms |
| the node's log line about a change to the frame that shows it | 0.55 to 1.85 ms (16 changes) | 0 to 25 ms |
| BMS_Fault period | mean 99.99 ms, longest gap 102.3 ms (85 frames) | mean 100 ± 1 ms, gaps below 125 ms |

The final run of bench/node_timing.py timed the BMS_Fault intervals at its observer over longer
runs at 500 kbit/s ([results/node_timing.json](../results/node_timing.json),
`periods.BMS_Fault` of each transport and condition, host timing as above): on an idle bus 1188
intervals over SocketCAN and 1184 over UDP, every one within 598 µs (SocketCAN) and 542 µs (UDP)
of 100 ms; under a flood at half the bus the longest was 115 ms over SocketCAN and 101 ms over
UDP, during downloads 113 ms and 101 ms. The two longest come from the SocketCAN flood and
download conditions, the only ones in which the host made the node give up ticks (15 in the
flood run, 16 and 6 in two of the three downloads, `runs[].tick_dropped`); no interval was a
cycle and a half or longer, and every one was below the test's 125 ms.

The faults of the pack, each applied after its test had passed and then removed (UDP):

| Fault | Command | To active | To no longer active | Evidence needed |
|---|---|---|---|---|
| CELL_OVERVOLTAGE | `sensor.set cell3 4300` | 199.6 ms | 199.0 ms | 10 cell readings |
| CELL_UNDERVOLTAGE | `sensor.set cell6 2400` | 200.1 ms | 198.2 ms | 10 cell readings |
| OVERTEMPERATURE | `sensor.set temp1 700` | 599.4 ms | 600.3 ms | 6 temperature readings |
| UNDERTEMPERATURE | `sensor.set temp2 -300` | 599.9 ms | 600.0 ms | 6 temperature readings |
| OVERCURRENT_DISCHARGE | `sensor.set current 280000` | 999.3 ms | 999.1 ms | 100 current readings |
| OVERCURRENT_CHARGE | `sensor.set current -150000` | 998.3 ms | 1000.2 ms | 100 current readings |
| SENSOR_CHANNEL_INVALID | `sensor.open cell5` | 99.7 ms | 99.0 ms | 1 cell reading |
| FRONT_END_NO_ANSWER | `sensor.offline on` | 100.2 ms | 98.4 ms | 1 sample |

The test sends each command just after a BMS_Fault frame, so the health job that sees the last
piece of evidence follows it closely: the times lie near k T for k pieces of evidence every T,
or near one health period where one piece decides. The test's bounds are (k − 1) T and
k T + 100 ms + 25 ms. A shorted bus (`pytest tests/system/test_faults.py -k shorted -rP`): from
the first BMS_Fault back on the bus, error passive healed after 989.1 / 989.1 ms and bus off
after 4987.9 / 4988.1 ms. bench/bus_off.py times the heal of CAN_BUS_OFF from another start,
the controller's return to error active, to the node's log line that the DTC is no longer
active: 4.90 to 5.01 s in its final run, the medians 4.94 to 4.96 s, over 600 trials of a short
and of 32 destroyed transmissions at 500, 125 and 1000 kbit/s over UDP
([results/bus_off.json](../results/bus_off.json), `bus_off_dtc_heal_ms`, a mixed figure: the
health task's 50 cycles and the host's scheduling of it).

The hardware watchdog model alone, with the platform's self-test program:

```bash
bin="$FCN_BUILD_ROOT/release/tests/platform/wdg_selftest"
for i in 1 2 3; do "$bin" kick-cost 1000000; done
for i in 1 2 3 4 5; do "$bin" expire 200; done
"$bin" kicked 200 3
```

- a kick took 93.6, 74.9 and 71.7 ns on average in three runs of 1,000,000 kicks each, the two
  clock reads around each call included; the longest single call took 1.5 to 10.4 ms, the host
  preempting the thread;
- not kicked after its seventh kick, the 200 ms watchdog reset the program with exit status 11
  200.903 to 202.311 ms after its last kick in five runs, as the parent saw the exit, with the
  model's own count at 200 ms each time;
- kicked every 50 ms for 3 s (62 kicks), the longest time the model counted without a kick was
  50.130 ms, it saw no host stall (`gaps` 0), and the run ended in order with status 0.

The size of the DTC state, with `sizeof` on the host and `arm-none-eabi-gcc -mcpu=cortex-m3
-mthumb`: `dtc_state_t` is 152 bytes on x86-64 and 148 on Cortex-M3, `dtc_event_config_t` 16 and `dtc_entry_t` 12 on both; `wdgm_state_t` is 204 and 168 bytes; the retained cell 36. At -Os for Cortex-M3, dtc.o has 880 bytes of code and wdgm.o 874, neither any data.

## How it is tested

| Test | What it shows |
|---|---|
| [tests/unit/test_dtc.c](../tests/unit/test_dtc.c) | every set, reset and kept rule of every status bit; the counter at its thresholds and at the extremes of its types; the count of failing cycles saturating at 255; the operation cycle, the clear, the records, counting and listing by mask; refused input; and random runs (300 random tables of 1 to 32 events, 1000 random steps each) compared after every step with a reference model written in the test from the bit definitions |
| [tests/unit/test_wdgm.c](../tests/unit/test_wdgm.c) | every local and global transition with its requirement number, the bounds of a correct count, the kick in every global status, every kind of damaged state making it STOPPED, 300 random configurations of 300 supervision cycles against a reference model written from the SWS's requirement tables, and 4 threads reporting 200,000 checkpoints each during supervision, every one counted once (also under ThreadSanitizer) |
| [tests/unit/test_fault.c](../tests/unit/test_fault.c) | the table of faults; every limit at the limit, just beyond, in the band and at its release value; invalid and unanswered readings; the debounce count of every sample fault; the health task's monitors; every global supervision status; every reset cause; the clear, its count compared across 2^32, and its order against the verdicts that carry the count of requests; the summary, with a fault that came and went within one period and the rises before and after a clear |
| [tests/unit/test_telemetry.c](../tests/unit/test_telemetry.c), [test_command.c](../tests/unit/test_command.c) | BMS_Fault byte by byte and through the E2E check, PackState and PackFaultActive, ResetCause for every cause, the clear requests, none by a tester that starts after another went away or comes back with a changed value |
| [tests/platform/test_posix_retained.c](../tests/platform/test_posix_retained.c), [wdg_selftest.c](../tests/platform/wdg_selftest.c) | every verdict of the cell at start, what a reset writes; the watchdog model in FreeRTOS programs run as children: expiry never before the timeout, a kicked watchdog that never resets, the start rules, the cell across real resets, `hang on` and `hang spin` |
| [tests/system/test_retained.py](../tests/system/test_retained.py) | the node with `--retained-file`: every verdict, every reason reported by name and value, a killed node, two nodes on one cell, an unusable path, and a watchdog reset of `wdg_selftest expire` that the node then reports |
| [tests/system/test_boot.py](../tests/system/test_boot.py), [test_update.py](../tests/system/test_update.py), [test_update_cuts.py](../tests/system/test_update_cuts.py) (phase 4) | the reset cause through the bootloader: a watchdog reset of the node started through fcn_boot is reported by the next start (BMS_NodeInfo, RESET_WATCHDOG) next to the hand-off; UPDATE_ROLLBACK active and confirmed in the first BMS_Fault after a ROLLBACK (a trial that crashes, one that hangs), a REJECT (a trial with a flipped bit) and a FALLBACK (a confirmed image with a flipped bit), and not in the start after that; after a cut in the rollback record, reported by the start that writes it again, and lost when the cut completed it ([update.md](update.md)) |
| [tests/system/test_faults.py](../tests/system/test_faults.py) | every fault of the table applied from outside: the right DTC in BMS_Fault, and only it, while it lasts, PackFaultActive and FAULT meanwhile, testFailed cleared and confirmedDTC kept after it, and the status byte, counter and fault detection counter in the statistics, both while the fault is active and after it, with two exceptions: SAMPLE_TIMEOUT, during which no sample and so no BMS_PackStatus comes, in BMS_PackStatus and in the statistics only once a stall the node survived has ended, and SUPERVISION_EXPIRED, which ends in a reset, which writes no statistics and leaves no after; a fault that comes and goes within one health period, active in its frame and logged; the clear while a fault is there and after it, a failure detected after the request surviving it and nothing from before the request in its frame; a shorted bus until bus off and the 5 s until its DTC heals; every reset cause a cell can record; `sensor.hang on` ending in a reset with status 11 and nothing else, then the start on the same cell (WATCHDOG, RESET_WATCHDOG until a clear) and after a power loss (POWER_ON); the supervision configuration of the design; one BMS_Fault per health period and every change of the DTCs in the frame of the period that made it, the frames counted exactly against the statistics and every one checked with the independent Profile 11 implementation; and, as timing tolerances, the time from a fault to its frame, from a hang to the reset, and from a change's log line to the frame that shows it, a stall of the sensor task that leaves two examinations without an alive indication survived (SAMPLE_TIMEOUT healed), and one that leaves three ending in a reset |
| [tests/system/test_diag_services.py](../tests/system/test_diag_services.py), [test_download.py](../tests/system/test_download.py), [test_download_faults.py](../tests/system/test_download_faults.py) (phase 5) | ReadDTCInformation over UDS: the DTCs of the fault table in its order, the lists and counts by mask against reportSupportedDTC; a cell over its voltage limit, after it heals and after ClearDiagnosticInformation, each time 0x19's status bytes against BMS_Fault's FaultActive and FaultConfirmed DTC by DTC, and after the clear a BMS_Fault with FaultClearCount 1 and nothing confirmed; the snapshot of a run without a fault against the statistics; UPDATE_ROLLBACK after a rollback of a downloaded image read through 0x19, in BMS_Fault, and gone after the clear ([diagnostics.md](diagnostics.md)) |
| every system test | the test harness fails any test whose node was reset without the test waiting for that reset with `wait_for_reset()`, and any test with a run that ended in order whose statistics show a kick missing: the hardware watchdog not started, kicks and withheld kicks that do not add up to the supervision cycles, or kicks the health task decided on that the watchdog did not count (`NodeProcess.lost_kicks()`); at the end of a test it stops the nodes that still run in order (SIGTERM), so that their kicks are checked too ([tests/conftest.py](../tests/conftest.py), for the nodes started through its fixtures; the few tests that start the node themselves assert its exit status and, after an orderly end, its kicks). So no node was reset without the test waiting for it, and in every run that ended in order every kick the health task decided on reached the hardware watchdog; neither shows that the kicks came often enough in a run shorter than the watchdog's 500 ms timeout or in the last 500 ms before an orderly stop, which switches the watchdog off |

Each module was challenged with planted defects when it was written: realistic mistakes in a
scratch copy, each of which a test must catch. The system tests of this page were challenged in
the same way. 22 defects, each planted alone in a scratch copy, and every one caught:

| Planted defect | Caught by |
|---|---|
| bus off heals after 5 health cycles instead of 50 | the fault detection counter of CAN_BUS_OFF in the statistics (every test that checks the DTCs) |
| a group the front end did not answer is also SENSOR_CHANNEL_INVALID | `sensor.offline`: a second DTC in BMS_Fault |
| a passed test also resets confirmedDTC (lib/dtc) | FaultConfirmed after the fault is gone |
| a passed test also resets the warning indicator (lib/dtc) | FaultWarning after the fault is gone |
| BMS_Fault carries the DTCs of the period before | PackFaultActive while bus off is active disagrees with BMS_Fault |
| PackFaultActive and FAULT follow the confirmed faults, not the active ones | BMS_PackStatus after the fault is gone |
| the hardware watchdog started with 5000 ms | the FATAL line of the hang's reset |
| the watchdog model resets with the reason of an assertion | the hang's exit status |
| RESET_WATCHDOG requests the warning indicator | the first BMS_Fault after a watchdog reset |
| the reset faults of the other causes are not passed at the start | their status bytes in the statistics |
| the clear carried out before the period's verdicts | the frame of the clear shows the channel fault from before the request (`test_a_failure_detected_after_a_clear_request_survives_the_clear`) |
| the clear carried out after the period's verdicts, whatever count of requests they carry | the failure after the request is erased (the same test; tests/unit/test_fault.c) |
| a fault that rose and fell within one period not kept for the summary | no BMS_Fault frame has the fault active (`test_a_fault_that_comes_and_goes_within_a_health_period_is_reported_all_the_same`; test_fault.c) |
| a clear that keeps the rises of its period before it for the summary | the frame of the clear has the channel fault from before the request active (`test_a_failure_detected_after_a_clear_request_survives_the_clear`; test_fault.c) |
| the log lines of a period only for a fault whose testFailed differs at its end | no line for a fault that came and went within the period, and none for the clear of a fault that the first piece of evidence after it decides again (the clear test of FRONT_END_NO_ANSWER and SENSOR_CHANNEL_INVALID) |
| the sensor task makes up the periods it missed after a stall | a stall of 241 ms, two examinations without an alive indication, resets the node ("30 alive in its last cycle"; the timing test of a stall within the failed tolerance, which reports it as a failure, not a timing miss) |
| the upper bound of the sensor entity widened to 65535 | the supervision configuration of a run without faults |
| a command carried out before the stream was lost kept as the reference for a clear request | tests/unit/test_command.c (a tester that starts after another, one back with a changed value) and the stream that stops in tests/system/test_telemetry.py (`clear_requests` 1) |
| the harness does not report a reset nobody waited for (three ways: in `unexpected_reset()`, in the fixture, in `wait_for_reset()`) | tests/python/test_node.py and test_harness.py |
| the health task never kicks the hardware watchdog | the harness's kick check (`NodeProcess.lost_kicks()`): of the 91 tests of test_smoke.py, test_retained.py, test_can_link.py, test_application.py and test_sensor_control.py outside the timing run, 66 pass against it with the harness of before this check, and 40 of those 66 now fail, each on its lost kicks |

## What phase 3 does not do, and what phases 4 and 5 add or leave out

- **Aging.** confirmedDTC and testFailedSinceLastClear stay set until a clear.
- **No storage in non-volatile memory: a limitation.** A reset loses every DTC, confirmed ones
  included, and every run is one operation cycle. The flash of phase 4 holds the images and the
  boot control metadata only. A product keeps at least the status byte and the count of failing
  cycles per DTC number in non-volatile memory (not the `dtc_state_t`, which holds a pointer to
  the table), restores them at the start and calls `dtc_start_operation_cycle()`; only then do
  confirmation thresholds above 1, a pendingDTC that outlives a cycle and the aging of
  confirmedDTC mean something. The faults of a start (the reset faults, UPDATE_ROLLBACK) are
  judged again at each start from the retained cell instead, and a tester sees them only in the
  run right after the reset or rollback that caused them (and not at all if that run never
  reaches its first BMS_Fault, see UPDATE_ROLLBACK above).
- **Boot attempts (phase 4)** are not counted in the retained cell, which a loss of supply
  erases: a trial that cuts its own supply would then never run out of attempts. lib/boot counts
  them in the flash, one program unit per attempt written before the trial starts
  ([update.md](update.md)); the cell's reserved words carry the bootloader's hand-off instead.
- **UDS (phase 5).** ReadDTCInformation (0x19: 0x01, 0x02, 0x0A) and ClearDiagnosticInformation
  (0x14, all groups) over ISO-TP expose the DTCs through the read functions above
  ([read access over UDS](#read-access-over-uds-phase-5)); BMS_Fault and the statistics are the
  other views, and CmdClearFaultsSeq the other clear. The other reports of 0x19 are not served.
- **Not implemented in lib/dtc:** healing of the warning indicator, snapshot (freeze-frame) and
  extended data records, occurrence counters, enable and storage conditions, an event memory
  smaller than the number of events, time-based debouncing, jump up, more than one kind of
  operation cycle, clearing a single DTC or group.
- **The node acts on no fault.** It has no contactor or current limit; the pack's master decides.
- **Not modelled:** a pin reset, the window watchdog's lower bound and the LSI clock's tolerance
  of the IWDG; the DTCs' failure-type bytes are all 0x00.
