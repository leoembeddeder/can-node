/* SPDX-License-Identifier: MIT */
/**
 * @file
 * What the application needs from the measurement front end of the battery module.
 *
 * On a board this would be a cell-monitor IC with temperature inputs and a current-sense
 * amplifier, read over SPI. On the host it is a model (platform/posix/posix_sensor.c,
 * docs/sensor-model.md) that produces plausible, slightly noisy values and that the test bench
 * can disturb through simulation control: hold a channel at a value, declare a channel open,
 * take the front end off line. The application cannot tell the model from hardware and has no
 * way to reach its simulation control.
 *
 * Each call reads one group of measurements. It returns false if the front end did not
 * answer; nothing is written then. A channel whose reading the front end itself declares
 * invalid (an open sense wire, for example) is returned with its bit set in the invalid
 * mask and an unspecified value, which the caller must not use.
 *
 * Units as in lib/bms/bms.h: millivolts, tenths of a degree Celsius, milliamperes (positive
 * when the pack is discharging). A value can lie anywhere in its type, outside what the sensor
 * can deliver included: checking that is the caller's job (bms_*_plausible()).
 *
 * All three functions may be called from one task only, the sensor task, and never from an
 * interrupt. They do not block: on the host each returns at once, except while the test bench
 * makes the front end hang (simulation control "sensor.hang", docs/sensor-model.md), a fault
 * of the hardware under which a read does not return until the fault is removed. That is the
 * fault the watchdogs are there for.
 */
#ifndef FCN_PLAT_SENSOR_H
#define FCN_PLAT_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "bms/bms.h"

/** Pack current in mA into *current_ma; *invalid true if the front end has no valid reading. */
bool plat_sensor_read_current(int32_t *current_ma, bool *invalid);

/** All cell voltages in mV; bit n of *invalid_mask marks cell n as not measurable. */
bool plat_sensor_read_cells(uint16_t cell_mv[BMS_CELLS], uint8_t *invalid_mask);

/** All temperatures in 0.1 degC; bit n of *invalid_mask marks sensor n as not measurable. */
bool plat_sensor_read_temps(int16_t temp_dc[BMS_TEMPS], uint8_t *invalid_mask);

#endif /* FCN_PLAT_SENSOR_H */
