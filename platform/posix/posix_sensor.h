/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The measurement front end of the host build: a model behind plat_sensor.h, and the
 * simulation control commands that disturb it ("sensor.*"). What the model produces and what
 * the commands do is described in docs/sensor-model.md and at the top of posix_sensor.c.
 *
 * The model knows nothing about FreeRTOS. Its read functions are called by the sensor task, its
 * commands are served by whichever thread calls posix_sim_service() (in the node, the CAN
 * hardware thread); the two share only atomics. The one place where a read needs the kernel,
 * the blocking wait of "sensor.hang on", is a function the node hands in
 * (posix_sensor_set_wait()).
 */
#ifndef FCN_POSIX_SENSOR_H
#define FCN_POSIX_SENSOR_H

#include <stdint.h>

/** The seed of a node started without --sensor-seed. */
#define POSIX_SENSOR_DEFAULT_SEED 1U

/**
 * Start the model from the beginning: every channel follows the model, the front end is on
 * line, every read counter is 0, and the noise comes from `seed` (any value; the same seed
 * gives the same readings). Registers the "sensor." commands with simulation control.
 *
 * Call before the thread that serves simulation control starts (posix_sim_register()), and
 * before the first read. Calling it again is allowed while neither thread uses the model, as
 * the tests do.
 */
void posix_sensor_start(uint32_t seed);

/** How a read waits while the front end hangs with "sensor.hang on": the read calls it again
 *  and again until the hang ends. */
typedef void (*posix_sensor_wait_fn)(void);

/**
 * Set the wait of "sensor.hang on". The node sets a function that blocks the calling task for
 * one tick (vTaskDelay(1)), so that the reading task is blocked, as a task whose driver waits
 * for an interrupt that never comes, and the tasks below it run on. NULL, the default, sleeps
 * the thread for 1 ms instead, which is all a test without a kernel needs. "sensor.hang spin"
 * never calls it. Kept by posix_sensor_start(). Call before the reading thread starts.
 */
void posix_sensor_set_wait(posix_sensor_wait_fn wait);

#endif /* FCN_POSIX_SENSOR_H */
