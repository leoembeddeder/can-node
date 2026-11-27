# The CAN protocol of the node

Every frame the node sends or accepts is defined in one file, [dbc/fcn.dbc](../dbc/fcn.dbc).
The C code that packs and unpacks the signals, [app/gen/can_db.h](../app/gen/can_db.h) and
[app/gen/can_db.c](../app/gen/can_db.c), is generated from it by
[tools/gen_can_db.py](../tools/gen_can_db.py), and the Python tools decode the bus with
cantools from the same file. This page describes the messages; the tables below are checked
against the DBC by `tests/python/test_gen_can_db.py`, so they cannot drift from it unnoticed.

## Conventions

- **Identifiers.** Base format, 11 bits. A lower identifier wins arbitration, so identifiers
  are handed out by urgency: the pack summary first, diagnostics last.
- **Bit numbering.** Every signal is little-endian ("Intel"). Bit 0 is the least significant
  bit of byte 0, bit 8 the least significant bit of byte 1, and so on; a signal's *start bit*
  is the position of its least significant bit, and bit *i* of its raw value is frame bit
  *start + i*.
- **Raw and physical values.** On the wire a signal is an integer, its *raw* value, in two's
  complement if it is signed. The physical value is `raw * factor + offset` in the unit given.
- **SNA.** Where a measurement can be missing, the signal's value table has an entry `SNA`
  ("signal not available"), always the raw value with all bits set. A receiver must check for
  it before scaling. Measurements outside the range of a signal are sent as the nearest end of
  the range, never as SNA and never wrapped.
- **Unused bits** are sent as 0 and ignored on reception.
- **Cyclic and event messages.** A cyclic message is sent every cycle time from a slot of the
  transmit queue that keeps only the newest value (`lib/can/can_txq.h`), so a busy bus delays
  data but never queues stale values. Event messages wait in a FIFO and are never replaced.

## Messages

| ID | Name | Sender | DLC | Cycle | E2E Data ID | Content |
|---|---|---|---|---|---|---|
| 0x010 | BMS_Fault | BMS | 8 | 100 ms | 0x0110 | the diagnostic trouble codes: active and confirmed faults, the most recent one, warning indicator, clears |
| 0x020 | BMS_PackStatus | BMS | 8 | 10 ms | 0x0120 | pack current and voltage, pack state, fault flag, valid flags |
| 0x030 | BMS_Heartbeat | BMS | 8 | 10 ms | none | period counter and tick count: the status frame of phase 1, unchanged |
| 0x040 | BMS_CellVoltages1 | BMS | 8 | 20 ms | 0x0140 | cells 1 to 4 |
| 0x041 | BMS_CellVoltages2 | BMS | 8 | 20 ms | 0x0141 | cells 5 to 8 |
| 0x050 | BMS_Temperatures | BMS | 8 | 100 ms | 0x0150 | four temperatures |
| 0x100 | TESTER_Command | TESTER | 8 | 100 ms | 0x0200 | telemetry on/off, clear-faults sequence |
| 0x600 | BMS_TaskStats | BMS | 8 | 200 ms | none | statistics of one task per frame, round robin |
| 0x610 | BMS_NodeInfo | BMS | 8 | 1000 ms | none | software version, active slot, reset cause, uptime |
| 0x611 | BMS_BootInfo | BMS | 8 | 1000 ms | none | how the bootloader started the image (confirmed or trial, attempt, the state change it made), the image's version, the confirm of a trial |
| 0x6F0 | TESTER_Ping | TESTER | 8 | event | none | any payload of 0 to 8 bytes, no signals |
| 0x6F1 | BMS_Pong | BMS | 8 | event | none | the payload of the ping, no signals |
| 0x7E0 | UDS_Request | TESTER | 8 | event | none | ISO 15765-2 frames of the tester's UDS requests, no signals ([below](#diagnostics-uds_request-and-uds_response)) |
| 0x7E8 | UDS_Response | BMS | 8 | event | none | ISO 15765-2 frames of the node's UDS responses, no signals |

The DBC gives ping and pong a DLC of 8, the most they carry; the frames themselves may be
shorter. `TESTER_Command` is sent cyclically although it carries commands, so that the node can
tell a tester that has gone silent from one that has nothing new to say.

**Bus load.** The node sends 327 cyclic frames per second, and they occupy at most 8.83 % of
the 500 kbit/s bus (the worst case of bit stuffing for 8-byte frames, plus the intermission);
the tester's command adds 10 frames per second, at most 0.27 %. From the DBC with:

```bash
python - <<'EOF'
import cantools
from fcn_tools import canbits
db = cantools.database.load_file("dbc/fcn.dbc")
bitrate = db.dbc.attributes["Baudrate"].value
for sender in ("BMS", "TESTER"):
    cyclic = [m for m in db.messages if m.cycle_time and m.senders == [sender]]
    frames = sum(1000 / m.cycle_time for m in cyclic)
    bits = sum(1000 / m.cycle_time * (canbits.worst_case_bits(False, False, m.length)
                                      + canbits.INTERMISSION_BITS) for m in cyclic)
    print(f"{sender}: {len(cyclic)} cyclic messages, {frames:.0f} frames/s, "
          f"at most {bits:.0f} bit/s = {100 * bits / bitrate:.2f} % of {bitrate} bit/s")
EOF
```

```
BMS: 9 cyclic messages, 327 frames/s, at most 44145 bit/s = 8.83 % of 500000 bit/s
TESTER: 1 cyclic messages, 10 frames/s, at most 1350 bit/s = 0.27 % of 500000 bit/s
```

The final run of bench/bus_load.py counted the frames of the node alone on the 500 kbit/s bus
in a run of 60 s, with their exact stuffed lengths ([results/bus_load.json](../results/bus_load.json)):
327.0 frames per second and 7.7 % of the bus, the intermission included, over SocketCAN and UDP
alike, below the worst case above and close to the 7.6 % that the DBC gives with typical
stuffing. The lengths and the load per frame are protocol data; the frames per second depend on
the host keeping the node's time.

## Signals

Start bit and length in bits; range is the physical range the DBC gives (`[min|max]`), values
are the entries of the signal's value table as raw value and name.

### 0x010 BMS_Fault

| Signal | Start bit | Length | Type | Factor | Offset | Unit | Range | Values | Meaning |
|---|---|---|---|---|---|---|---|---|---|
| E2E_Crc | 0 | 8 | unsigned | 1 | 0 |  | 0..255 |  | E2E CRC |
| E2E_Counter | 8 | 4 | unsigned | 1 | 0 |  | 0..14 |  | E2E counter |
| FaultActive | 16 | 17 | unsigned | 1 | 0 |  | 0..131071 |  | bit n: the DTC of fault n has testFailed at the end of the period, or its testFailed rose during it (after the clear, in the period of a clear): the fault is present, or was since the last frame |
| FaultConfirmed | 33 | 17 | unsigned | 1 | 0 |  | 0..131071 |  | bit n: the DTC of fault n has confirmedDTC, the fault occurred since the start or the last clear |
| FaultMostRecent | 50 | 5 | unsigned | 1 | 0 |  | 0..16 | 0 CELL_OVERVOLTAGE, 1 CELL_UNDERVOLTAGE, 2 OVERTEMPERATURE, 3 UNDERTEMPERATURE, 4 OVERCURRENT_DISCHARGE, 5 OVERCURRENT_CHARGE, 6 SENSOR_CHANNEL_INVALID, 7 FRONT_END_NO_ANSWER, 8 SAMPLE_TIMEOUT, 9 CAN_BUS_OFF, 10 CAN_ERROR_PASSIVE, 11 SUPERVISION_EXPIRED, 12 RESET_WATCHDOG, 13 RESET_ASSERT, 14 RESET_STACK_OVERFLOW, 15 RESET_MALLOC_FAILED, 16 UPDATE_ROLLBACK, 31 NONE | the fault whose DTC went to testFailed most recently since the start or the last clear |
| FaultWarning | 55 | 1 | unsigned | 1 | 0 |  | 0..1 |  | a DTC requests the warning indicator: a fault of the pack's limits or its measurement occurred |
| FaultClearCount | 56 | 8 | unsigned | 1 | 0 |  | 0..255 |  | clears carried out since the start, modulo 256 |

Fault *n* is entry *n* of FaultMostRecent's value table; [app/fault.h](../app/fault.h) gives the
DTC number of each, and the application checks every entry against its own numbering at compile
time. Bits 12 to 15 are unused. Phase 4 added fault 16, UPDATE_ROLLBACK, which made the two
masks 17 bits wide: FaultActive keeps its start bit (its bits 0 to 15 are where they were in
phase 3), and FaultConfirmed, FaultMostRecent and FaultWarning moved up by one, two and two
bits, into the bits that were unused (54 and 55). The node's health task, the one task that changes
the DTCs, sends the message once in each of its 100 ms periods, after the last change it made in
the period: every change of the DTCs goes out in the frame of the period that made it, and a
fault that became active and passed again within the period is active in that frame. It is
protected under Data ID 0x0110 (see [End-to-end protection](#end-to-end-protection)) and is not
switched off by CmdTelemetryOn. [faults.md](faults.md) describes the faults, their DTCs and how
a tester clears them.

### 0x020 BMS_PackStatus

| Signal | Start bit | Length | Type | Factor | Offset | Unit | Range | Values | Meaning |
|---|---|---|---|---|---|---|---|---|---|
| E2E_Crc | 0 | 8 | unsigned | 1 | 0 |  | 0..255 |  | E2E CRC |
| E2E_Counter | 8 | 4 | unsigned | 1 | 0 |  | 0..14 |  | E2E counter |
| PackCurrent | 16 | 16 | signed | 0.01 | 0 | A | -327.68..327.67 |  | filtered, positive while discharging; valid only if PackCurrentValid |
| PackVoltage | 32 | 16 | unsigned | 0.001 | 0 | V | 0..65.535 |  | sum of the filtered cells; valid only if PackVoltageValid |
| PackState | 48 | 2 | unsigned | 1 | 0 |  | 0..2 | 0 STARTUP, 1 NORMAL, 2 FAULT | STARTUP until every channel has delivered a reading, FAULT while one is invalid or a fault is active |
| PackFaultActive | 50 | 1 | unsigned | 1 | 0 |  | 0..1 |  | a fault is active: a bit of FaultActive is set in the last BMS_Fault (a DTC had testFailed at the end of that period, or its testFailed rose during it) |
| PackCurrentValid | 51 | 1 | unsigned | 1 | 0 |  | 0..1 |  | PackCurrent holds a valid measurement |
| PackVoltageValid | 52 | 1 | unsigned | 1 | 0 |  | 0..1 |  | every cell is valid, so PackVoltage is their sum |

Bits 12 to 15 (the high nibble of byte 1) and 53 to 63 are unused.

### 0x030 BMS_Heartbeat

| Signal | Start bit | Length | Type | Factor | Offset | Unit | Range | Values | Meaning |
|---|---|---|---|---|---|---|---|---|---|
| PeriodCounter | 0 | 32 | unsigned | 1 | 0 |  | 0..4294967295 |  | 10 ms periods the node has run since the start; the first frame carries 1. A period skipped after an overrun is not counted, so the counter never jumps; TickCount shows the time |
| TickCount | 32 | 32 | unsigned | 1 | 0 | tick | 0..4294967295 |  | the node's tick count when the frame was published (1 tick = 1 ms) |

This is the status frame of phase 1 bit for bit: bytes 0 to 3 and 4 to 7, each a 32-bit
number with the low byte first. Every bus test of phase 1 counts on it, and the unit tests
hold the codec to it. A value is published into a slot of the transmit queue, so a counter
value missing on the bus was replaced by its successor before it could go out, and the node
counts it as superseded.

### 0x040 BMS_CellVoltages1

| Signal | Start bit | Length | Type | Factor | Offset | Unit | Range | Values | Meaning |
|---|---|---|---|---|---|---|---|---|---|
| E2E_Crc | 0 | 8 | unsigned | 1 | 0 |  | 0..255 |  | E2E CRC |
| E2E_Counter | 8 | 4 | unsigned | 1 | 0 |  | 0..14 |  | E2E counter |
| Cell1Voltage | 16 | 12 | unsigned | 0.001 | 1 | V | 1..5.094 | 4095 SNA | filtered cell voltage |
| Cell2Voltage | 28 | 12 | unsigned | 0.001 | 1 | V | 1..5.094 | 4095 SNA | |
| Cell3Voltage | 40 | 12 | unsigned | 0.001 | 1 | V | 1..5.094 | 4095 SNA | |
| Cell4Voltage | 52 | 12 | unsigned | 0.001 | 1 | V | 1..5.094 | 4095 SNA | |

1 mV per bit from 1.000 V: raw = millivolts - 1000. Raw 4094 is 5.094 V, raw 4095 is SNA.

### 0x041 BMS_CellVoltages2

| Signal | Start bit | Length | Type | Factor | Offset | Unit | Range | Values | Meaning |
|---|---|---|---|---|---|---|---|---|---|
| E2E_Crc | 0 | 8 | unsigned | 1 | 0 |  | 0..255 |  | E2E CRC |
| E2E_Counter | 8 | 4 | unsigned | 1 | 0 |  | 0..14 |  | E2E counter |
| Cell5Voltage | 16 | 12 | unsigned | 0.001 | 1 | V | 1..5.094 | 4095 SNA | as in BMS_CellVoltages1 |
| Cell6Voltage | 28 | 12 | unsigned | 0.001 | 1 | V | 1..5.094 | 4095 SNA | |
| Cell7Voltage | 40 | 12 | unsigned | 0.001 | 1 | V | 1..5.094 | 4095 SNA | |
| Cell8Voltage | 52 | 12 | unsigned | 0.001 | 1 | V | 1..5.094 | 4095 SNA | |

### 0x050 BMS_Temperatures

| Signal | Start bit | Length | Type | Factor | Offset | Unit | Range | Values | Meaning |
|---|---|---|---|---|---|---|---|---|---|
| E2E_Crc | 0 | 8 | unsigned | 1 | 0 |  | 0..255 |  | E2E CRC |
| E2E_Counter | 8 | 4 | unsigned | 1 | 0 |  | 0..14 |  | E2E counter |
| Temp1 | 16 | 8 | unsigned | 1 | -40 | degC | -40..214 | 255 SNA | filtered temperature |
| Temp2 | 24 | 8 | unsigned | 1 | -40 | degC | -40..214 | 255 SNA | |
| Temp3 | 32 | 8 | unsigned | 1 | -40 | degC | -40..214 | 255 SNA | |
| Temp4 | 40 | 8 | unsigned | 1 | -40 | degC | -40..214 | 255 SNA | |

1 degC per bit from -40 degC: raw = degrees + 40. Bits 48 to 63 are unused.

### 0x100 TESTER_Command

| Signal | Start bit | Length | Type | Factor | Offset | Unit | Range | Values | Meaning |
|---|---|---|---|---|---|---|---|---|---|
| E2E_Crc | 0 | 8 | unsigned | 1 | 0 |  | 0..255 |  | E2E CRC |
| E2E_Counter | 8 | 4 | unsigned | 1 | 0 |  | 0..14 |  | E2E counter |
| CmdTelemetryOn | 16 | 1 | unsigned | 1 | 0 |  | 0..1 |  | 0 silences the protected cyclic messages (0x020, 0x040, 0x041, 0x050); the others go on. 1 until a valid command has been carried out |
| CmdClearFaultsSeq | 24 | 8 | unsigned | 1 | 0 |  | 0..255 |  | the node clears its DTCs when it changes between two commands it carries out while the stream stays valid; the first command carried out after the stream started or was lost only sets the reference |

### 0x600 BMS_TaskStats

| Signal | Start bit | Length | Type | Factor | Offset | Unit | Range | Values | Meaning |
|---|---|---|---|---|---|---|---|---|---|
| TaskIndex | 0 | 4 | unsigned | 1 | 0 |  | 0..15 |  | which task the frame describes |
| TaskPriority | 4 | 4 | unsigned | 1 | 0 |  | 0..15 |  | its FreeRTOS priority |
| TaskCpuShare | 8 | 16 | unsigned | 0.01 | 0 | % | 0..100 |  | its share of the CPU time since the start |
| TaskStackPeak | 24 | 16 | unsigned | 1 | 0 | byte | 0..65535 |  | deepest stack use seen, saturated |
| TaskOverruns | 40 | 8 | unsigned | 1 | 0 |  | 0..255 |  | periods with a missed deadline, saturated |

One task per frame, the next task in the next frame, so a node with *n* tasks describes each
of them every n * 200 ms.

### 0x610 BMS_NodeInfo

| Signal | Start bit | Length | Type | Factor | Offset | Unit | Range | Values | Meaning |
|---|---|---|---|---|---|---|---|---|---|
| SwMajor | 0 | 8 | unsigned | 1 | 0 |  | 0..255 |  | software version |
| SwMinor | 8 | 8 | unsigned | 1 | 0 |  | 0..255 |  | |
| SwPatch | 16 | 8 | unsigned | 1 | 0 |  | 0..255 |  | |
| ActiveSlot | 24 | 2 | unsigned | 1 | 0 |  | 0..1 | 0 A, 1 B, 3 SNA | the firmware slot the node runs from, as the bootloader's hand-off names it; SNA for a node started without the bootloader |
| ResetCause | 26 | 6 | unsigned | 1 | 0 |  | 0..62 | 0 POWER_ON, 10 REQUESTED, 11 WATCHDOG, 12 POWER_LOSS, 13 IMAGE_FAULT, 20 ASSERT, 21 STACK_OVERFLOW, 22 MALLOC_FAILED, 63 SNA | the values of `plat_reset_reason_t` (platform/include/platform.h): the reset cause of this start, read from the retained-RAM cell (`plat_reset_cause()`); POWER_ON for a start without a reset before it; SNA for a value that is none of these |
| UptimeSeconds | 32 | 32 | unsigned | 1 | 0 | s | 0..4294967295 |  | seconds since the start |

Every bit of BMS_NodeInfo is in use, so what phase 4 adds about the running image is in a
message of its own, BMS_BootInfo.

### 0x611 BMS_BootInfo

| Signal | Start bit | Length | Type | Factor | Offset | Unit | Range | Values | Meaning |
|---|---|---|---|---|---|---|---|---|---|
| BootMode | 0 | 2 | unsigned | 1 | 0 |  | 0..2 | 0 NONE, 1 CONFIRMED, 2 TRIAL | how the bootloader started the image; NONE: started without the bootloader (no hand-off), every other signal but ConfirmState then 0 and BootEventSlot SNA |
| BootAttempt | 2 | 2 | unsigned | 1 | 0 |  | 0..3 |  | for a TRIAL the attempt this start is, 1 to 3; 0 otherwise |
| BootEvent | 4 | 3 | unsigned | 1 | 0 |  | 0..7 | 0 NONE, 1 ADOPT, 5 ROLLBACK, 6 REJECT, 7 FALLBACK | the state change the bootloader made before this start (the values of lib/boot's events); ROLLBACK, REJECT and FALLBACK are the DTC UPDATE_ROLLBACK |
| ConfirmState | 8 | 3 | unsigned | 1 | 0 |  | 0..4 | 0 NOT_NEEDED, 1 WAITING, 2 CONFIRMING, 3 CONFIRMED, 4 FAILED | the confirm of a trial: not needed (no trial), waiting for its 5 s of good health, being written, written (the trial is the active image), or failed (it will be rolled back after its attempts) |
| BootEventSlot | 11 | 2 | unsigned | 1 | 0 |  | 0..1 | 0 A, 1 B, 3 SNA | the slot BootEvent acted on: the adopted slot (ADOPT), the rejected trial's (ROLLBACK, REJECT), or the slot fallen back to, whose image is the confirmed one again (FALLBACK; the other slot's image is rejected); SNA for none |
| ImageVersionMajor | 16 | 8 | unsigned | 1 | 0 |  | 0..255 |  | the version of the running image from its header (lib/image: major.minor.patch); 0 without the bootloader |
| ImageVersionMinor | 24 | 8 | unsigned | 1 | 0 |  | 0..255 |  | |
| ImageVersionPatch | 32 | 16 | unsigned | 1 | 0 |  | 0..65535 |  | |
| ConfirmSeconds | 48 | 8 | unsigned | 1 | 0 | s | 0..255 |  | the seconds of good health a trial has counted towards its confirm (the confirm is made at 5), saturated |

The health task sends it once a second, in its periods 2, 12, 22, ... (200 ms after each
BMS_NodeInfo period, in an even period without BMS_TaskStats: a period still sends at most two
frames, app/app_tasks.h). Everything but ConfirmState and ConfirmSeconds comes from the
bootloader's hand-off at the start (platform/include/plat_retained.h) and stays as it is for the
run. Bits 7, 13 to 15 and 56 to 63 are unused. Not protected end to end, like the other
diagnostic messages of the node. [app/update.h](../app/update.h) describes the confirm.

## Diagnostics: UDS_Request and UDS_Response

Since phase 5 the two identifiers at the end of the table carry one ISO-TP connection (ISO
15765-2: normal addressing, physical only, classical CAN, every frame 8 bytes): the tester's UDS
requests on 0x7E0, the node's responses on 0x7E8. The DBC has no signals for them: what a frame
carries is the transport's N_PCI and a piece of a message, which [diagnostics.md](diagnostics.md)
describes with the services. On the wire:

- every frame the node sends has DLC 8, its unused bytes 0xCC; the node accepts a tester's frame
  whose DLC covers what its first byte says, padded or not;
- the node's flow control is `30 10 00 CC CC CC CC CC` (ContinueToSend, a block of 16 consecutive
  frames, no separation time), after a first frame and after every 16th consecutive frame; it
  answers a first frame of more than 1026 bytes with `32 00 00 CC CC CC CC CC` (overflow);
- a responsePending is `03 7F <SID> 78 CC CC CC CC`.

Both are event messages: the node's frames go through the event FIFO of the transmit queue, never
replaced, and are confirmed to the transport in the order of their transmit-done reports. Their
identifiers are the highest on the bus, the lowest priority, so a download yields the bus to
every periodic frame; the node does not listen to functional requests (0x7DF): it handles 0x7E0
alone. A TransferData of 1024 bytes of data is 147 frames of the tester's and 11 of the node's:
10 flow controls and the response.

## End-to-end protection

The CRC-15 of a CAN frame protects it on the wire. It cannot see what happens above the wire:
a frame lost or repeated in a buffer or a gateway, a stale value sent again and again by a task
that hangs, data sent under the identifier of another message, a byte changed in a software
buffer. The protected messages therefore carry an end-to-end header that the producer of the
data writes and its consumer checks: AUTOSAR E2E Profile 11, variant 11A, from the AUTOSAR E2E
Protocol Specification (AUTOSAR_FO_PRS_E2EProtocol R24-11, "PRS", section 6.12), with the
library interface of AUTOSAR_CP_SWS_E2ELibrary R24-11 ("LIB", section 8.3.11). Table 6.45 of
the PRS lists which fault each mechanism covers:

- the CRC detects corruption;
- the Data ID detects insertion, masquerading and incorrect addressing;
- the counter detects repetition, loss, delay and wrong order;
- a check in every receive period detects a sender that has stopped.

Protected are the messages whose DBC attribute `FcnE2eDataId` is not -1: BMS_Fault (0x010),
BMS_PackStatus (0x020), BMS_CellVoltages1 (0x040), BMS_CellVoltages2 (0x041), BMS_Temperatures
(0x050) and TESTER_Command (0x100). The node protects the first five with
[lib/e2e](../lib/e2e/e2e_p11.h) and checks the sixth. On the Python side,
[fcn_tools.decode](../tools/fcn_tools/decode.py) checks the first five and the system tests'
tester protects the sixth, both with
[fcn_tools.e2e](../tools/fcn_tools/e2e.py), a second implementation written from the PRS
without looking at lib/e2e.

### Layout

Variant 11A (PRS_E2E_00540): CRC in byte 0, counter in the low nibble of byte 1, Data ID mode
BOTH. Bit numbers are as in the DBC, bit 0 being the least significant bit of byte 0:

| Bits | Field |
|---|---|
| 0..7 (byte 0) | CRC |
| 8..11 (byte 1, low nibble) | counter, 0..14 |
| 12..15 (byte 1, high nibble) | payload; the protection leaves it alone |
| 16..63 (bytes 2..7) | payload |

The 16-bit Data ID is never sent. Both ends know it from the DBC, and it enters the CRC.
Fig. 6.91 of the PRS draws the bits of byte 1 the other way round; its formulas (Fig. 6.96,
Fig. 6.108) and its test vectors put the counter in the low nibble.

The PRS's variant definition (PRS_E2E_00540, item 4) names an unused-bit pattern of 0xFF:
payload bits that no signal uses would be sent as 1. This project sends them as 0, as every
other unused bit (see [Conventions](#conventions)): the codec writes 0 there, and the protection
covers whatever is there. It is a deviation from the variant's definition, not from the
check, which treats those bits as data either way.

### CRC

PRS_E2E_00508 names the CRC library's Crc_CalculateCRC8, the CRC-8-SAE J1850 routine
(polynomial 0x1D). Fig. 6.97 calls it only with Crc_IsFirstCall = FALSE, the first time with
the start value 0xFF, and XORs the final result with 0xFF. For such calls the library first
XORs the start value with 0xFF (SWS_Crc_00041). The net algorithm is therefore:

- polynomial 0x1D, most significant bit first;
- start value 0x00;
- no final XOR.

That is CRC-8/GSM-A in the CRC catalogues (check value 0x37 for "123456789"). It is not the
catalogue's CRC-8/SAE-J1850, which starts at 0xFF, inverts the result and has the check value
0x4B. [e2e_p11.c](../lib/e2e/e2e_p11.c) writes this derivation next to its start value.

The bytes are fed in this order (PRS_E2E_00505): Data ID low byte, Data ID high byte, bytes 1
to 7 of the message. Byte 0, the CRC itself, is left out; the counter is covered.

PRS Tables 6.48 and 6.49 give the first two messages of a sender with Data ID 0x123 and an
all-zero payload: `CC 00 00 00 00 00 00 00` and `91 01 00 00 00 00 00 00`. The C unit tests
reproduce these two and the 11C vectors of Tables 6.50 and 6.51. A 11C message is a 11A message
with Data ID high byte 0 and the Data ID nibble in the payload nibble (PRS_E2E_00506,
PRS_E2E_00541). The Python tests reproduce all five vectors, including Table 6.52, which puts
the CRC at bit 64 of a 16-byte message.

**Data IDs.** If two Data IDs leave the CRC register in the same state after their two bytes,
they protect every message identically: a message of one passes the check of the other.
PRS_E2E_UC_00072 asks, for data with ASIL B to D requirements, that no other data element have a
Data ID with the same CRC over its two bytes; for ASIL A the weaker PRS_E2E_UC_00073 applies.
`fcn_tools.e2e.data_id_crc()` computes that value, and `tools/gen_can_db.py` computes it again
with its own `data_id_crc()` and refuses a DBC in which two protected messages share it (the
values of this DBC are in the table below). Each of the 256 values is shared by exactly 8 of
the 2 048 11-bit identifiers. 0x123, the Data ID of the PRS vectors, shares its value (0xBC)
with 0x0A0, 0x2BB, 0x338, 0x496, 0x515, 0x68D and 0x70E
(`tests/python/test_e2e.py::test_data_id_crc_classes`).

### Counter

**Sender.** The sender starts at 0 (E2E_P11ProtectInit, SWS_E2E_00555) and counts 0, 1, ...,
14, 0, ... (PRS_E2E_00504, Fig. 6.99). 15 is reserved and never sent (PRS 6.12.1.1). The node
keeps one counter per protected message, and advances it only when it builds a value of that
message: while CmdTelemetryOn is 0 the counters stand still, so a receiver sees the next
counter when the telemetry comes back.

**Receiver.** The receiver keeps the counter of the last message whose CRC was right. It starts
at 14 (E2E_P11CheckInit, SWS_E2E_00556), so that the first message of a sender that has just
started, counter 0, is one step ahead.

**Step.** The step from the stored to the received counter is their difference, plus 15 when it
is negative (Fig. 6.110, "taking into wrap around 0xE"). So 14 to 0 is one step, and 0 to 14
is fourteen steps.

**max_delta_counter** (1..14) is the largest step that still counts as good. 1 tolerates no lost
message; n tolerates n − 1 lost in a row.

### What the receiver does

The receiver checks once for every message that arrives, in the order of arrival. It also
checks once with "no new data" (data NULL, length 0) for every period of the message in which
none arrived. That second kind of call is how a timeout becomes visible (PRS 6.2.3.2;
PRS_E2E_UC_00325, "should be invoked at least once within FTTI"); without it a sender that
stops is never noticed. The first rule that applies decides (PRS_E2E_00516, Fig. 6.110):

| Status (value) | When | Stored counter | Input to the state machine (SWS_E2E_00608) | The message |
|---|---|---|---|---|
| NONEWDATA (0x01) | nothing arrived in the period | kept | E2E_P_NONEWDATA: neither OK nor error | none; the window holds one OK fewer |
| ERROR (0x07) | CRC wrong: damaged, or the message of another Data ID | kept | E2E_P_ERROR: an error | discarded |
| ERROR (0x07) | CRC right, counter 15 (this project's rule, see below) | kept | E2E_P_ERROR: an error | discarded |
| WRONGSEQUENCE (0x40) | step above max_delta_counter: more messages lost than tolerated, or an old message | set to the received one | E2E_P_WRONGSEQUENCE: neither | not used; the receiver follows the sender from here |
| REPEATED (0x08) | step 0: the counter of the last message again | unchanged | E2E_P_REPEATED: neither | not used as a new value |
| OK (0x00) | step 1 | set to the received one | E2E_P_OK | new and intact |
| OKSOMELOST (0x20) | step 2 .. max_delta_counter | set to the received one | E2E_P_OK | new and intact, some lost before it |
| return code other than E2E_E_OK | a mistake of the caller, a damaged state, or a message of the wrong length | kept | E2E_P_ERROR (SWS_E2E_00558) | not judged |

The status alone never decides whether a value is used. The application uses a received value
only while the state machine of its message is VALID, and its substitute value otherwise. With
max_delta_counter 1, one damaged message costs two verdicts: its own ERROR, and WRONGSEQUENCE for
the next message, whose counter is two steps after the last one received
(`tests/unit/test_e2e_sm.c::test_profile_11_through_the_state_machine`). With the
max_delta_counter of 2 that the node uses for TESTER_Command, the next message is OKSOMELOST
instead, and the damaged one costs only its own verdict.

### The state machine

This is the state machine of PRS 6.20 with transitToInvalidExtended = false (Fig. 6.131),
through the library functions E2E_SMCheck (SWS_E2E_00340) and E2E_SMCheckInit (SWS_E2E_00353).
It keeps the last `window_size` verdicts of a message and counts the OKs and the errors among
them after every verdict outside NODATA (E2E_SMAddStatus, PRS_E2E_00466, Fig. 6.133). In NODATA
no transition depends on the counts, the figure skips them, and both stay 0. REPEATED,
WRONGSEQUENCE and NONEWDATA count as neither; they act only by keeping OKs out of the window.

| State (PRS_E2E_00914) | Meaning | Leaves for | When |
|---|---|---|---|
| NODATA | nothing received since the initialisation | INIT | on a verdict that is neither ERROR nor NONEWDATA; the window is cleared (E2E_SMClearStatus, PRS_E2E_00467), that verdict included |
| INIT | data arrives, not enough of it yet | VALID | errors ≤ max_error_state_init and OKs ≥ min_ok_state_init |
| | | INVALID | errors > max_error_state_init |
| VALID | the data may be used | INVALID | errors > max_error_state_valid or OKs < min_ok_state_valid; the window is kept |
| INVALID | communication not good enough | VALID | errors ≤ max_error_state_invalid and OKs ≥ min_ok_state_invalid; otherwise every ERROR clears the window |

Only VALID means "use the data". The parameters (`e2e_sm_config_t`) are checked on every call:

| Parameter | Range | Meaning |
|---|---|---|
| window_size | 1..16 | verdicts looked at, the same in every state |
| min_ok_state_init, min_ok_state_valid, min_ok_state_invalid | 1..window_size | OKs needed to enter VALID from INIT or INVALID, and to stay VALID |
| max_error_state_init, max_error_state_valid, max_error_state_invalid | 0..window_size − 1 | errors tolerated in that state |

What follows from them, counted in check cycles of the message:

- **Start.** VALID at the earliest with the (min_ok_state_init + 1)-th message without error after
  the initialisation: the message that leaves NODATA is not counted.
- **Timeout.** From VALID with a window full of OKs, the state machine reaches INVALID in the
  (window_size − min_ok_state_valid + 1)-th period in a row without a message,
  (window_size − min_ok_state_valid) cycle times after the first one (PRS 6.20.4, item 2).
  Repetitions and counter jumps starve the window in the same way.
- **Corruption.** INVALID as soon as the window holds max_error_state_valid + 1 errors.
- **Recovery.** VALID again once the window holds min_ok_state_invalid OKs and at most
  max_error_state_invalid errors. Errors from before INVALID stay in the window until they are
  pushed out. Every ERROR in INVALID clears the window, so recovery needs min_ok_state_invalid
  OKs after the last error.
- **A sender that never starts** leaves the receiver in NODATA. Without the extended variant
  there is no transition from NODATA to INVALID (PRS_E2E_00675). The data is not used in NODATA
  either, but a "never received" diagnosis has to look for NODATA itself.

The R24-11 state machine has parameters that are fixed here (PRS_E2E_00912):

- one window size instead of WindowSizeInit, WindowSizeValid and WindowSizeInvalid. With equal
  sizes, E2E_SMClearRemainingStatus (PRS_E2E_00607) never clears anything;
- clearFromValidToInvalid = false;
- transitToInvalidExtended = false;
- combinedNoDataInitCount = false.

### The node's receiver of TESTER_Command

[app/command.c](../app/command.c), run by the receive task, checks every TESTER_Command frame when
it arrives and, through the receive cycle of [app/can_io.c](../app/can_io.c), checks with "no new
data" at the end of every 100 ms period (the message's cycle time) in which none arrived. Its
parameters ([app/command.h](../app/command.h)):

| Parameter | Value | Consequence |
|---|---|---|
| max_delta_counter | 2 | one lost frame in a row still counts as new data (OKSOMELOST) |
| window_size | 4 | |
| min_ok / max_error, INIT | 2 / 1 | the third good frame of a new tester is the first one carried out |
| min_ok / max_error, VALID | 2 / 1 | VALID is left in the third period in a row without a frame (200 ms after the first one), or with the second error within four verdicts |
| min_ok / max_error, INVALID | 3 / 0 | after a timeout, the third good frame, with no error in the window, is the first one carried out again |

A command is carried out only if, after its frame's check and the state machine's step, the
state machine is VALID and the frame itself was new and intact (OK or OKSOMELOST). A repeated
frame, one out of sequence, a damaged one, one with another Data ID or another length, and
every frame while the state machine is NODATA, INIT or INVALID are counted and ignored. Before
the first command carried out the telemetry is on; afterwards it keeps the last command's
CmdTelemetryOn, also after a timeout. A change of CmdClearFaultsSeq from one command carried
out to the next is a request to clear the DTCs only while the state machine stayed VALID
between the two: the first command carried out after the stream started, or after any step
that left it not VALID, sets the reference and requests nothing, so that another tester, or a
pack master after its own reset, does not clear the node's DTCs by starting once its stream
has been lost. A sender whose stream is not lost in between, one that is silent for at most
one period (two, if its E2E counter happens to go on in sequence), is taken for the one before
it: a pack master that resets that quickly and comes back with another value requests a
clear. A tester that changed its value while its stream was lost sees FaultClearCount
unchanged in BMS_Fault and changes it again. The node's statistics (`--stats-json`, `"command"`) count
every status, the checks, the commands carried out and ignored, and give the state machine's
state. [tests/system/test_telemetry.py](../tests/system/test_telemetry.py) sends each kind of
bad frame and a stream that stops, and checks those counts exactly.

### Limits

- **Hamming distance 3.** Over the 64 bits of an 8-byte message, the 0x1D polynomial detects
  every error of 1 or 2 bits but not every error of 3. Of the 41 664 patterns of 3 bits, 198 go
  undetected, 0.48 %. This was found by trying every pattern
  (`tests/python/test_e2e.py::test_hamming_distance_of_an_8_byte_message_is_3`).
- **Length limits.** 0x1D keeps a distance of 4 only up to 13 data bits, and of 3 up to 247.
  0x2F, the polynomial of Profiles 2 and 22, keeps a distance of 4 up to 119 data bits, and so
  over a whole 8-byte message (`test_hamming_distance_limits_of_the_two_profile_polynomials`).
  The PRS states a Hamming distance only for Profiles 1 and 2 (PRS 9.5). Where a distance of 4
  is required, the profile to use is Profile 22, which keeps the bus layout of Profile 2
  (PRS 6.13.1), not this one.
- **Counter aliasing.** The counter repeats after 15 messages. When exactly 14 in a row are lost,
  the next one looks like a repetition; after 15 lost, it looks like the next message; a longer
  outage can look like a shorter one. Long outages are caught by the cycles without data and
  the state machine, not by the counter.
- **Not authentication.** Whoever knows the Data ID can make a valid message.
- **Detection only.** E2E reports faults; what to do with data that cannot be trusted is the
  application's decision (PRS 9.4). Loss and delay are detected only when the check runs every
  period (PRS 4.1.2). One undetected erroneous message must not by itself violate a safety goal
  (PRS_E2E_UC_00170).

### Where this project decided

1. **A received counter of 15 gives ERROR, and the stored counter is kept.**
   - Fig. 6.110 has no test for it. Taken literally, 15 after a stored 14 is a step of 1 and so
     OK; the 15 is stored, and the sender's next correct message, counter 0, is then judged
     REPEATED.
   - A message with a correct CRC and counter 15 cannot come from a sender that follows the
     profile. It is damage the CRC did not catch, or a sender with another counter scheme
     (Profiles 1 and 2 count 0..15).
   - Profile 1 refuses such a counter with E2E_E_INPUTERR_WRONG (PRS Fig. 6.58). A return code is
     not used here, because it means a mistake of the caller and leaves the previous status in
     place, while this is a fault of the received data.
2. **Input errors.**
   - E2E_E_INPUTERR_NULL and E2E_E_INPUTERR_WRONG as the PRS assigns them (Fig. 6.94,
     Fig. 6.106).
   - E2E_E_INPUTERR_WRONG also for a configuration out of range: data_len 2..8,
     max_delta_counter 1..14, and the state machine's limits above.
   - E2E_E_INPUTERR_WRONG also for a state that no sequence of calls produces: a counter above
     14, a window index outside the window.
   - Nothing is written on an error return.
3. **DEINIT is 0.** In `e2e_sm_state_t`, DEINIT has the value 0. The library gives VALID the value
   0x00 (SWS_E2E_00344), so a state block in zeroed memory that was never initialised would read
   as VALID. All other numeric values are the library's.
4. **Only variant 11A in C.** The C module has the fixed 11A layout for 2 to 8 bytes.
   `fcn_tools.e2e` implements the configurable offsets and both Data ID modes of the PRS, which
   is what lets it run all five published vectors.
5. **Unused bits are 0,** not the 0xFF of the variant's definition (see [Layout](#layout)).

### Code and tests

- `lib/e2e/e2e_p11.h`, `lib/e2e/e2e_sm.h`, `lib/e2e/e2e.h`: the C interface. Each function names
  the AUTOSAR function and the requirements it implements.
- `tools/fcn_tools/e2e.py`: the independent Python implementation (`Config`, `protect`, `check`,
  `Sender`, `Receiver`, `data_id_crc`), written from the specification text.
- `tests/unit/test_e2e_p11.c` (24 tests) and `tests/unit/test_e2e_sm.c` (20 tests) cover the
  published vectors, every status and every transition at its exact cycle, every input error, a
  random run of the state machine against a reference model, and the profile and the state
  machine together.
- `tests/python/test_e2e.py` covers the published vectors and the CRC library's examples, and
  puts 20 193 requests to the C code through `tests/tools/e2e_cli.c`, each compared with the
  Python answer in every field, including the state left behind.
- `tests/system/test_telemetry.py` checks every protected frame the running node sends with
  `fcn_tools.e2e`, and the node's receiver with frames of every kind.

### Checking the bus

[fcn_tools.decode](../tools/fcn_tools/decode.py) decodes frames with cantools from the DBC and
checks every protected message with a Profile 11 receiver of its own, in the order the frames
come. `python -m fcn_tools.monitor` prints the frames on a bus as they arrive; here the first
five frames of a node started on the default bus after the monitor:

```bash
python -m fcn_tools.monitor udp:29500 --count 5
```

```
1795054987.108949  020  BMS_PackStatus      e2e OK             c=0   PackCurrent=1.95 A  PackVoltage=29.569 V  PackState=NORMAL  PackFaultActive=0  PackCurrentValid=1  PackVoltageValid=1
1795054987.109182  030  BMS_Heartbeat       PeriodCounter=1  TickCount=12 tick
1795054987.109389  040  BMS_CellVoltages1   e2e OK             c=0   Cell1Voltage=3.698 V  Cell2Voltage=3.693 V  Cell3Voltage=3.702 V  Cell4Voltage=3.693 V
1795054987.109633  041  BMS_CellVoltages2   e2e OK             c=0   Cell5Voltage=3.696 V  Cell6Voltage=3.698 V  Cell7Voltage=3.692 V  Cell8Voltage=3.697 V
1795054987.109935  050  BMS_Temperatures    e2e OK             c=0   Temp1=25 degC  Temp2=25 degC  Temp3=26 degC  Temp4=25 degC
```

The first frame on the bus is BMS_PackStatus, not the heartbeat: both are published in the first
period, and the lower identifier wins arbitration.

`--id 0x020` (repeatable) prints only those identifiers, `--seconds` bounds the run, and
`--max-delta-counter` sets the receivers' tolerance (default 1, so that every lost frame shows).
A monitor that joins a running bus judges the first frame of each message against the
receiver's initial counter, so that first verdict says nothing.

What the DBC fixes about it, and the generator checks:

- A message is protected if its attribute `FcnE2eDataId` is a 16-bit Data ID, and not if it is
  -1, the default. Protected messages are the cyclic ones in the table above with a Data ID.
- Every protected message starts with the Profile 11A header: `E2E_Crc` in byte 0 and
  `E2E_Counter` in bits 0 to 3 of byte 1, both unsigned and unscaled. The payload starts at
  bit 16; bits 12 to 15 are unused.
- Each Data ID is 0x100 plus the message's identifier. Data ID and identifier differ on
  purpose: code that fed the identifier into the CRC instead of the Data ID would then fail
  every check rather than pass unnoticed.
- The Profile 11 CRC of the two Data ID bytes is different for every protected message, so a
  frame of one cannot pass the check of another when their payloads agree:

| Message | Data ID | CRC of the Data ID bytes |
|---|---|---|
| BMS_Fault | 0x0110 | 0xA9 |
| BMS_PackStatus | 0x0120 | 0x68 |
| BMS_CellVoltages1 | 0x0140 | 0xF7 |
| BMS_CellVoltages2 | 0x0141 | 0xBB |
| BMS_Temperatures | 0x0150 | 0x43 |
| TESTER_Command | 0x0200 | 0x3A |

  Computed with `tools/gen_can_db.py`'s `data_id_crc()` (polynomial 0x1D, start value 0x00,
  no final XOR, over the low and then the high byte of the Data ID).

## The generated codec

`app/gen/can_db.h` gives, for every message, `CAN_DB_<MSG>_ID`, `_DLC`, `_CYCLE_MS`,
`_E2E_PROTECTED` and, for protected messages only, `_E2E_DATA_ID`. For every message with
signals it declares a struct of the raw values and two functions:

```c
bool can_db_bms_pack_status_pack(const can_db_bms_pack_status_t *msg, uint8_t *data,
                                 size_t capacity);
bool can_db_bms_pack_status_unpack(can_db_bms_pack_status_t *msg, const uint8_t *data,
                                   size_t len);
```

`pack()` writes all DLC bytes, every bit outside the signals as 0, and masks each struct
member to its signal's length: a value cannot spill into a neighbour, but it is not saturated
either. `unpack()` sign-extends signed signals. Both refuse NULL pointers and a buffer of the
wrong size without touching the output. Every signal has `_START`, `_LENGTH`, `_RAW_MIN`,
`_RAW_MAX`, its factor and offset as exact fractions (`_FACTOR_NUM` / `_FACTOR_DEN`,
`_OFFSET_NUM` / `_OFFSET_DEN`) and one constant per value-table entry, `_SNA` among them.

The structs hold raw values on purpose. The conversion from millivolts or tenths of a degree
belongs to the code that has the values and knows how to round, saturate and when to send SNA;
it states what it assumes about the DBC at compile time, one relation per assertion,

```c
/* millivolts = raw + 1000 */
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_FACTOR_NUM == 1, "1/1000 V per bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_FACTOR_DEN == 1000, "1/1000 V per bit");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_OFFSET_NUM == 1, "offset 1 V");
_Static_assert(CAN_DB_BMS_CELL_VOLTAGES1_CELL1_VOLTAGE_OFFSET_DEN == 1, "offset 1 V");
```

so that a change of the DBC it does not follow stops the build. (Joined into one assertion
with `&&`, two comparisons that expand to the same tokens, such as `FACTOR_NUM == 1` and
`OFFSET_DEN == 1`, are reported by clang-tidy's `misc-redundant-expression`.)

The generated code is code of the node: it is compiled with the project's warnings as errors
and checked by the MISRA addon and clang-tidy like the rest of `app/`. It is tested in three
ways. `tests/unit/test_can_db.c` holds it to frames worked out by hand and checks round trip,
field isolation, sign extension and masking for every signal. `tests/python/test_gen_can_db.py`
compares it with cantools on random raw values and random frames for every message, through
`tests/tools/can_db_cli.c`. And the same Python test generates, compiles and compares the code
for a DBC with signals of every length from 1 to 32 bits, signed and unsigned, each starting at
two or more bit positions within a byte, and signed and unsigned signals starting at every bit
position within a byte.

## Changing the DBC

1. Edit `dbc/fcn.dbc`.
2. Run `scripts/codegen-check.sh --update`. It regenerates `app/gen/can_db.h`,
   `app/gen/can_db.c` and the test table `tests/tools/can_db_table.c`, or stops with the list
   of rules the DBC breaks.
3. Update the tables on this page and the frames worked out by hand in
   `tests/unit/test_can_db.c`; `scripts/check.sh` fails until both agree with the DBC.

`scripts/codegen-check.sh` without `--update` changes nothing and fails with a diff while a
generated file is out of date; CI and `scripts/check.sh` run it.
