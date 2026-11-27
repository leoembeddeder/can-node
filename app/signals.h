/* SPDX-License-Identifier: MIT */
/**
 * @file
 * From the quantities the node works with to the raw values of its CAN signals: the one place
 * where a unit of lib/bms meets a factor and an offset of dbc/fcn.dbc.
 *
 * The generated codec (app/gen/can_db.h) carries raw values only and leaves converting,
 * rounding, saturating and choosing "signal not available" to the code that knows what a
 * value means. That is this file. Every conversion checks at compile time what it assumes
 * about the DBC (factor, offset, SNA code, raw range), one relation per _Static_assert in
 * signals.c, so that a change of the DBC that the arithmetic here does not follow breaks the
 * build instead of the data on the bus.
 *
 * ROUNDING. A conversion that divides rounds to the nearest raw value, and a value exactly
 * between two raw values away from zero: the rounding of the filters in lib/bms. The
 * conversion of a signed quantity is then odd-symmetric like them (-x gives -raw(x)).
 *
 * RANGES. A valid value beyond what its signal can carry is sent as the nearest value the
 * signal can carry (saturation), as the comments of the DBC say. "Signal not available" (SNA)
 * is sent for a value that is not valid, and only then: a valid value never turns into SNA by
 * being large. Every function is total: any value of its argument types has a defined result.
 *
 * CONTEXT. Pure functions without state: any task, any interrupt.
 */
#ifndef FCN_SIGNALS_H
#define FCN_SIGNALS_H

#include <stdbool.h>
#include <stdint.h>

#include "bms/bms.h"

/** The SNA codes of the cell and temperature signals (checked against the DBC in signals.c). */
#define SIGNALS_CELL_SNA        4095U
#define SIGNALS_TEMPERATURE_SNA 255U

/** PackCurrent (0.01 A per bit) from milliamperes: round(current_ma / 10), saturated to the
 *  signal's -32768..32767 (+-327.68 A; the sensor's range is +-300 A). */
int16_t signals_pack_current(int32_t current_ma);

/** PackVoltage (1 mV per bit) from millivolts, saturated at 65535 (65.535 V; eight plausible
 *  cells give at most 40 V). */
uint16_t signals_pack_voltage(uint32_t pack_mv);

/** A CellNVoltage signal (1 mV per bit, from 1000 mV) from millivolts: SIGNALS_CELL_SNA if
 *  `valid` is false; otherwise cell_mv - 1000, where a cell below 1000 mV is sent as 1000 mV
 *  (raw 0) and one above 5094 mV as 5094 mV (raw 4094). */
uint16_t signals_cell_voltage(uint16_t cell_mv, bool valid);

/** A TempN signal (1 degC per bit, from -40 degC) from tenths of a degree Celsius:
 *  SIGNALS_TEMPERATURE_SNA if `valid` is false; otherwise round(temp_dc / 10) + 40, where a
 *  temperature below -40 degC is sent as -40 degC (raw 0) and one above 214 degC as 214 degC
 *  (raw 254). */
uint8_t signals_temperature(int16_t temp_dc, bool valid);

/** PackState: the number of `state` (the DBC's value table has the numbers of bms_state_t).
 *  A value that is none of the three gives FAULT. */
uint8_t signals_pack_state(bms_state_t state);

/** TaskCpuShare (0.01 % per bit): round(10000 * task_us / total_us), at most 10000 (100 %);
 *  0 while total_us is 0. Both arguments in microseconds of the run-time counter. */
uint16_t signals_cpu_share(uint64_t task_us, uint64_t total_us);

/** `value`, or `max` if it is larger: for the counters and sizes the DBC gives a fixed width
 *  (TaskStackPeak, TaskOverruns, TaskIndex, TaskPriority). */
uint32_t signals_saturate(uint32_t value, uint32_t max);

#endif /* FCN_SIGNALS_H */
