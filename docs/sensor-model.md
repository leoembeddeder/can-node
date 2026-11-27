# The sensor front-end model

A battery-management node measures its module through a front end: on a board, a cell-monitor
IC with temperature inputs and a current-sense amplifier, read over SPI. The host build has a
model instead ([posix_sensor.c](../platform/posix/posix_sensor.c)) behind the interface the
application would use on hardware ([plat_sensor.h](../platform/include/plat_sensor.h)). This
page says what the model produces, how a test disturbs it, and what it leaves out, so that a
result about "the measurements" can be read for what it is.

## What the application sees

Three calls, one per group of measurements: the pack current, the eight cell voltages, the four
temperatures. Units are those of [lib/bms](../lib/bms/bms.h): mA (positive when the pack is
discharging), mV, and tenths of a degree Celsius. A call returns false when the front end does
not answer, and then writes nothing. Each channel also has an invalid flag, for a reading the
front end itself declares invalid (an open sense wire, for example). The value of such a
channel is unspecified, and the application must not use it.

The application's sensor task reads the current in every 10 ms period, the cells in every
second period and the temperatures in every tenth. The calls never block, unless the test bench
makes the front end hang (`sensor.hang`, below).

## The model

| Channel | Value | Noise |
|---|---|---|
| `current` | the load profile below, one step per current reading | up to ±150 mA |
| `cell1` .. `cell8` | open-circuit voltage − current × internal resistance, rounded to the nearest mV | up to ±2 mV |
| `temp1` .. `temp4` | a fixed temperature per sensor | up to ±0.2 °C |

**No clock.** The model has no time of its own: it advances with the readings taken from it.
One current reading is one step of the load profile, which is 10 ms at the rate the sensor task
reads the current. The cells see the profile's current at the latest current reading (the true
current, without the current sensor's noise), or the first step's before any current reading.
Reading *n* of a channel, counted from the start of the node and including readings the front
end did not answer, is a function of the seed, the channel and *n* alone. A cell reading also
depends on how many current readings came before it. Three things follow, and the tests check
each:

- the same seed gives the same readings, however well or badly the host keeps time;
- a fault on one channel changes the readings of no other channel;
- once a fault is cleared, the readings are those of a run without it: the model runs on
  through every fault, the front end being off line included.

**The load profile** repeats every 2000 current readings (20 s at 10 ms):

| Current readings | Duration at 10 ms | Current | Stands for |
|---|---|---|---|
| 0 .. 299 | 3 s | 2 A | the module at rest, its own electronics |
| 300 .. 1099 | 8 s | 45 A | steady discharge |
| 1100 .. 1249 | 1.5 s | 160 A | peak discharge |
| 1250 .. 1699 | 4.5 s | 45 A | steady discharge |
| 1700 .. 1999 | 3 s | −80 A | charging (regenerative braking) |

**The cells** differ a little from one another, as in a real module, so that a value that lands
on the wrong channel shows:

| Cell | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| Open-circuit voltage (mV) | 3702 | 3698 | 3705 | 3695 | 3700 | 3703 | 3697 | 3701 |
| Internal resistance (mΩ) | 1.80 | 1.90 | 1.75 | 2.10 | 1.85 | 1.95 | 1.80 | 2.00 |

Under the 160 A peak, cell 4 drops 336 mV to 3359 mV; while charging at 80 A, cell 3 rises
by 140 mV to 3845 mV. The temperatures are 25.1, 24.8, 25.6 and 24.5 °C. Every value the model
produces without a fault lies within what the sensors can deliver, the limits of the plausibility
check in lib/bms (0 to 5000 mV, −40.0 to 125.0 °C, ±300 A).

**Noise.** Each reading gets noise drawn from a triangular distribution, the sum of two uniform
draws, so values near the model's are more likely than the extremes. Reading *n* of a channel
draws from [SplitMix64](https://prng.di.unimi.it/splitmix64.c) (Steele, Lea and Flood, 2014):
its output number *n* + 1 for a key made from the seed and the channel. That counter-based
draw is what makes a reading independent of every other channel and of the order of the calls.

**The seed** is given with `--sensor-seed N` (0 to 4294967295; default 1). `sensor.status`
reports it, as does the node's log line at start-up.

## Faults

Faults are applied from outside through [simulation control](can-simulation.md#simulation-control),
as a test bench works on a real device. The application has no hook for them and cannot tell a
fault from real hardware trouble. Channels are named `current`, `cell1` to `cell8` and `temp1`
to `temp4`; `cell1` is bit 0 of the invalid mask, and the cell signal `Cell1Voltage` on the bus.

| Command | Effect | Reply | On a real bench |
|---|---|---|---|
| `sensor.set <channel> <value>` | the channel reports exactly `value`, as a valid reading, without noise | `<channel>=<value> t_ns=<time>` | a cell simulator drives a voltage; a stuck converter |
| `sensor.open <channel>` | the front end flags the channel invalid | `<channel>=open t_ns=<time>` | an open sense wire |
| `sensor.offline on\|off` | `on`: the front end answers no read at all | `offline=on t_ns=<time>` | the SPI bus fails, the front end is in reset |
| `sensor.hang on\|spin\|off` | `on` and `spin`: a read does not return until `off`; `on` blocks the reading task, `spin` keeps it on the CPU (below) | `hang=on t_ns=<time>` | a transfer that never completes: the driver waits for its interrupt (`on`) or polls a busy flag (`spin`) for ever |
| `sensor.clear [<channel>]` | the channel follows the model again; without a channel, every channel does, the front end is on line and does not hang | `<channel>=model t_ns=<time>` or `cleared=all offline=off hang=off t_ns=<time>` | |
| `sensor.status` | nothing | `seed=<n> offline=on\|off current_reads=<n> cell_reads=<n> temp_reads=<n> unanswered=<n>`, one `<channel>=model\|open\|<held value>` per channel, then `hang=off\|on\|spin hung=<n>` | |

- A channel is in one mode at a time: following the model, held at a value, or open. Each
  `set` or `open` replaces what the channel had before; the off-line state is separate and
  covers every channel.
- A held value can be anything the channel's type holds, also a value the sensor could not
  deliver: −2147483648 to 2147483647 for the current, 0 to 65535 for a cell, −32768 to 32767
  for a temperature. That is what tests the application's plausibility check. Anything else is
  refused with `a value for <channel> is a whole number from <lo> to <hi>, not '<text>'`.
- Holding the current changes its reading only: the cells keep following the true current.
- An open channel reports the value the model gives it, a plausible number. An application
  that ignored the flag would publish it, and only the flag can tell the test that it did not.
- Off line, every read returns false. The read is still counted (`*_reads`, and `unanswered`),
  and the model runs on.
- **The hang** is the hardware fault behind the watchdog tests: a task that stops because its
  driver never gets an answer. While it is on, every read waits at its start, and returns only
  after `sensor.hang off` or `sensor.clear`. With `on` the read waits the way a driver waits
  for its transfer-complete interrupt: the node blocks the sensor task one tick at a time
  (`vTaskDelay(1)`), so the tasks below it go on running, the health task included, and only
  something that watches the sensor task itself can see that it stopped: the watchdog manager,
  or the processing task's timeout on the samples that no longer come. With `spin` it waits
  the way a driver polling a busy flag does: the sensor task (priority 6 since phase 5) never gives up the
  CPU as far as the scheduler can tell, so every task below it starves, the processing,
  transmit and health tasks with it, and with the health task everything it does, such as
  kicking the hardware watchdog. (On the host the thread sleeps 1 ms between its looks at the
  hang, so that it does not burn a host core; the scheduler cannot see that sleep.)
  `hung` counts the reads that had to wait, from the moment each began to; a read that is
  waiting now is counted, so `hung` going up says that the sensor task is stuck. A read that
  waited is taken once the hang ends and counted then in `*_reads`: the model has no clock, so
  the readings go on with the next number, and the off-line state at that moment decides
  whether it is answered.
- `t_ns` is `CLOCK_MONOTONIC`, the clock of `plat_time_ns()`, at the moment the command took
  effect, so a test can measure from a fault to its consequence on the bus.
- A wrong command is answered with `err` and a message: `usage: ...` for a wrong number of
  arguments or an argument that is not one of the command's words (`usage: sensor.hang
  on|spin|off`), or a word of more than 23 characters; `unknown channel '<name>' (current,
  cell1..cell8, temp1..temp4)`, or the range message above. It changes nothing.

The commands are served by the CAN hardware thread, the thread that serves all of simulation
control, while the sensor task reads. The two share only atomics: one 64-bit word per channel
holds its mode and held value together. A read in progress while a command is applied sees the
channel's setting before the command or after it, never a mix of the two, and the first read
that starts after the reply was sent sees the new setting.

[`fcn_tools.simctl.SimControl`](../tools/fcn_tools/simctl.py) has a method per command:
`sensor_status()`, `sensor_set()`, `sensor_open()`, `sensor_offline()`, `sensor_hang()` (`"on"`,
`"spin"`, `"off"`, or `True` and `False`) and `sensor_clear()`. Each sets a state rather than
adding to one, so the client sends it again after a timeout (see `REPEATABLE`).

## What is not modelled

- **No state of charge.** The open-circuit voltages are constant; the model does not integrate
  the charge that flows. The profile is net discharge, and a real module's voltages would fall
  as it ran.
- **No thermal behaviour.** The temperatures do not follow the load.
- **No time.** The profile advances per current reading. An application that read the current
  at another rate would play it faster or slower.
- **No converter errors** other than the faults above: no gain or offset error, no drift, no
  noise spikes, no correlated faults (a broken sense wire that disturbs both neighbouring cells,
  as it does on real cell-monitor ICs).
- **No conversion time.** A read returns at once; a real front end needs time for each
  conversion and for the SPI transfer, which the sensor task's execution time would include.
- **No balancing,** and no open-wire detection: the model is told that a wire is open.

## Tests

- [tests/platform/test_posix_sensor.c](../tests/platform/test_posix_sensor.c): determinism
  (same seed, same readings; another seed, other noise), the profile and its boundaries, the
  cells against current × resistance, the noise per channel, each fault, every reply, the hang
  of a read in a second thread (it takes no reading until the hang ends and then returns the
  reading a run without the hang gives first), and the commands served from a second thread
  while the test reads (also run under ThreadSanitizer by `scripts/test-tsan.sh`).
- [tests/platform/wdg_selftest.c](../tests/platform/wdg_selftest.c) (`hang on`, `hang spin`):
  the hang under the scheduler. With `on` a task below the reading task keeps running; with
  `spin` it does not run at all until the hang ends.
- [tests/python/test_simctl.py](../tests/python/test_simctl.py): the client methods.
- [tests/system/test_sensor_control.py](../tests/system/test_sensor_control.py): the option and
  the commands in the running node; the hang in the running node: with `on` the sensor task
  stops reading while the tasks below it run on (the processing task logs its timeout, the
  health task finds the sensor task gone in its supervision), and the hardware watchdog resets
  the node; with `spin` no task below the sensor task runs, SIGTERM is not served, and the
  hardware watchdog resets the node all the same (exit status 11 both times).
- [tests/system/test_faults.py](../tests/system/test_faults.py): what the application makes of
  the hang: the DTCs it reports in BMS_Fault before the reset, the time to the reset, and the
  cause the next start on the same retained cell reports.
- [tests/system/test_telemetry.py](../tests/system/test_telemetry.py): what the node makes of the
  model. The decoded telemetry stays within the model's values and noise and follows the load
  profile's step from 2 A to 45 A; every channel opened or held beyond its sensor's range, and
  the front end off line, shows as "not available" (or a cleared valid flag) and a FAULT pack
  state in every frame that carries a reading taken after the command, and no longer in those
  that carry a reading taken after `sensor.clear` (the test tells the readings apart by the
  model's read counters in `sensor.status`).
