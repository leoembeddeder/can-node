"""The sensor front-end model in the running node: the --sensor-seed option, and the sensor.*
commands of simulation control served by the node.

What the model produces, that the same seed gives the same readings, and every reply of the
commands are tested without a node in tests/platform/test_posix_sensor.c; what the application
makes of the readings belongs to the tests of the application."""
from __future__ import annotations

import signal
import time

import pytest

from fcn_tools.node import EXIT_OK, RESET_WATCHDOG
from fcn_tools.simctl import SENSOR_CHANNELS, SimError


def test_the_node_serves_the_sensor_commands_with_the_seed_it_was_given(start_node):
    node = start_node("--sensor-seed", "4294967295")
    with node.sim() as sim:
        status = sim.sensor_status()
        assert status["seed"] == 4294967295
        assert status["offline"] == "off"
        assert all(status[channel] == "model" for channel in SENSOR_CHANNELS)

        sim.sensor_set("cell3", 4300)
        sim.sensor_open("temp2")
        sim.sensor_set("current", -400000)
        sim.sensor_offline(True)
        status = sim.sensor_status()
        assert (status["cell3"], status["temp2"], status["current"], status["offline"]) == \
            (4300, "open", -400000, "on")

        with pytest.raises(SimError, match="unknown channel 'cell9'"):
            sim.sensor_open("cell9")
        with pytest.raises(SimError, match="a value for cell1 is a whole number from 0 to 65535"):
            sim.sensor_set("cell1", 70000)

        sim.sensor_clear()
        status = sim.sensor_status()
        assert status["offline"] == "off"
        assert all(status[channel] == "model" for channel in SENSOR_CHANNELS)
    assert node.stop() == EXIT_OK


def test_without_the_option_the_seed_is_1(start_node):
    node = start_node()
    with node.sim() as sim:
        assert sim.sensor_status()["seed"] == 1
    assert node.stop() == EXIT_OK


# ------------------------------------------------------------------------- sensor.hang

def wait_until(condition, seconds: float = 5.0, what: str = "") -> None:
    """Poll `condition` until it holds; fail after `seconds`. Not a timing tolerance: the wait
    only bounds how long a test waits for something that must happen."""
    deadline = time.monotonic() + seconds
    while not condition():
        assert time.monotonic() < deadline, f"not within {seconds} s: {what}"
        time.sleep(0.01)


def hang_the_sensor_task(sim, mode: str) -> int:
    """Make the front end hang once the sensor task has read for a while; return the current
    readings taken by the moment the sensor task was seen stuck in its read."""
    wait_until(lambda: sim.sensor_status()["current_reads"] > 20, what="the sensor task reads")
    assert sim.sensor_hang(mode)["hang"] == mode
    wait_until(lambda: sim.sensor_status()["hung"] == 1, what="the sensor task is stuck")
    return sim.sensor_status()["current_reads"]


#: How long a test waits for the watchdog's reset after the sensor task got stuck: the node's
#: own bound is 0.8 s (app/app.c, SUPERVISION); the rest is room for a slow host. Not a timing
#: tolerance: it only bounds the wait (tests/system/test_faults.py measures the time).
RESET_WAIT_S = 10.0


def test_a_hanging_front_end_stops_the_sensor_task_but_not_the_tasks_below_it(start_node):
    """"on": the sensor task waits blocked inside its read and takes no reading; the tasks
    below it run on: the processing task times out on the samples that no longer come, the
    health task finds the sensor task gone in its supervision, stops kicking the hardware
    watchdog, and the watchdog resets the node."""
    node = start_node()
    with node.sim() as sim:
        reads = hang_the_sensor_task(sim, "on")
        time.sleep(0.2)
        status = sim.sensor_status()
        assert (status["current_reads"], status["hung"], status["hang"]) == (reads, 1, "on")
    assert node.wait_for_reset(timeout=RESET_WAIT_S) == RESET_WATCHDOG
    assert "FATAL watchdog: not kicked for" in node.stderr()
    log = node.log()
    assert "no sample from the sensor task" in log          # the processing task timed out
    assert "supervision: sensor expired" in log             # and so did the health task


def test_a_spinning_front_end_starves_every_task_below_the_sensor_task(start_node):
    """"spin": the sensor task never gives up the CPU, so the health task (priority 2) cannot
    see SIGTERM and cannot kick the hardware watchdog any more: the watchdog resets the node,
    with nothing in the log about it, since no task below the sensor task ran."""
    node = start_node()
    with node.sim() as sim:
        reads = hang_the_sensor_task(sim, "spin")
        node.send_signal(signal.SIGTERM)
        assert sim.sensor_status()["current_reads"] == reads
    assert node.wait_for_reset(timeout=RESET_WAIT_S) == RESET_WATCHDOG
    assert "FATAL watchdog: not kicked for" in node.stderr()
    assert "supervision:" not in node.log() and "stopping after" not in node.log()
