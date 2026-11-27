# SPDX-License-Identifier: MIT
"""fcn_tools.simctl, the client of the node's simulation control channel, against a small
server written here from the protocol in platform/posix/posix_sim.h. (The node's own side of
the protocol is tested in tests/platform/test_posix_sim.c, the two together in the system
tests.)"""
from __future__ import annotations

import socket
import threading
import time

import pytest

from fcn_tools.simctl import (FLASH_CUT_MAX, FLASH_SEED_MAX, HANG_MODES, REPEATABLE,
                              SENSOR_CHANNELS, SimControl, SimError)

LOOPBACK = "127.0.0.1"


class FakeNode:
    """Answers each request with what `replies` maps its command to. A value is the text after
    the token; a callable gets the request's token and command and returns that text, None to
    stay silent, or a list of whole datagrams to send instead. Requests are served one after
    the other, as the node does, and every one is recorded."""

    def __init__(self, replies, port: int = 0):
        self.replies = replies
        self.requests: list[str] = []
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.bind((LOOPBACK, port))
        self._socket.settimeout(0.05)
        self.port = self._socket.getsockname()[1]
        self._run = True
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        while self._run:
            try:
                data, sender = self._socket.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            token, _, command = data.decode().partition(" ")
            self.requests.append(command)
            reply = self.replies.get(command.split(" ")[0], "err unknown command")
            if callable(reply):
                reply = reply(token, command)
            if isinstance(reply, list):
                for datagram in reply:
                    self._socket.sendto(datagram.encode(), sender)
            elif reply is not None:
                self._socket.sendto(f"{token} {reply}".encode(), sender)

    def close(self) -> None:
        self._run = False
        self._thread.join(timeout=2)
        self._socket.close()


@pytest.fixture
def fake_node():
    nodes: list[FakeNode] = []

    def _start(replies, port: int = 0) -> FakeNode:
        node = FakeNode(replies, port)
        nodes.append(node)
        return node

    yield _start
    for node in nodes:
        node.close()


def test_a_reply_is_returned_as_its_key_value_pairs(fake_node):
    node = fake_node({"ping": "ok pid=4242"})
    with SimControl(node.port) as sim:
        assert sim.request("ping") == {"pid": "4242"}
        assert sim.ping() == 4242
    assert node.requests == ["ping", "ping"]


def test_can_status_converts_numbers_and_keeps_words(fake_node):
    node = fake_node({"can.status": "ok state=passive tec=128 rec=0 fault=noack tx=17 busy_ns=123456"})
    with SimControl(node.port) as sim:
        assert sim.can_status() == {"state": "passive", "tec": 128, "rec": 0, "fault": "noack",
                                    "tx": 17, "busy_ns": 123456}


def test_commands_carry_their_arguments(fake_node):
    node = fake_node({"can.fault": "ok fault=short", "can.inject": "ok tx_errors=3"})
    with SimControl(node.port) as sim:
        assert sim.can_fault("short") == {"fault": "short"}
        assert sim.can_inject("tx", 3) == {"tx_errors": "3"}
    assert node.requests == ["can.fault short", "can.inject tx 3"]


def test_an_error_reply_raises_with_the_nodes_message(fake_node):
    node = fake_node({"can.fault": "err unknown fault 'lightning' (none, short, dominant, noack)"})
    with SimControl(node.port) as sim:
        with pytest.raises(SimError, match="unknown fault 'lightning'"):
            sim.can_fault("lightning")
    assert len(node.requests) == 1          # an error is an answer: it is not asked again


def test_a_reply_with_another_token_is_not_taken_for_the_answer(fake_node):
    # The node answers with the token of an earlier request (a late reply to an attempt the
    # client has given up on), and never with this request's token.
    node = fake_node({"ping": lambda token, command: ["t0 ok pid=1", "x9 ok pid=2"]})
    with SimControl(node.port, timeout=0.2) as sim:
        with pytest.raises(SimError, match="no reply"):
            sim.request("ping", retries=0)
    assert node.requests == ["ping"]


def test_a_late_reply_to_an_earlier_attempt_is_skipped_for_the_right_one(fake_node):
    def answer_late_then_right(token, command):
        if token == "t1":
            return None                       # the first attempt goes unanswered ...
        return ["t1 ok pid=1", f"{token} ok pid=2"]   # ... and its reply comes late

    node = fake_node({"ping": answer_late_then_right})
    with SimControl(node.port, timeout=0.1) as sim:
        assert sim.ping() == 2
    assert node.requests == ["ping", "ping"]


def test_only_the_node_can_answer(fake_node):
    node = fake_node({"ping": lambda token, command: None})
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stranger, \
            SimControl(node.port, timeout=0.2) as sim:
        client = sim._socket.getsockname()
        stranger.sendto(b"t1 ok pid=666", client)     # the token the client is about to use
        with pytest.raises(SimError, match="no reply"):
            sim.request("ping", retries=0)


def test_a_silent_node_is_asked_again_and_then_reported(fake_node):
    node = fake_node({"ping": lambda token, command: None})
    with SimControl(node.port, timeout=0.1) as sim:
        with pytest.raises(SimError, match="no reply to 'ping'"):
            sim.request("ping", retries=2)
    assert node.requests == ["ping", "ping", "ping"]


@pytest.mark.parametrize("command", ["can.inject tx 5", "can.something_new"])
def test_a_command_that_must_not_run_twice_is_sent_once_and_waited_for(fake_node, command):
    node = fake_node({command.split(" ")[0]: lambda token, command: None})
    started = time.monotonic()
    with SimControl(node.port, timeout=0.1) as sim:
        with pytest.raises(SimError, match="no reply"):
            sim.request(command, retries=2)
    assert time.monotonic() - started >= 0.3          # (retries + 1) x timeout, all for one
    assert node.requests == [command]


def test_a_late_answer_to_a_command_that_must_not_run_twice_is_still_taken(fake_node):
    # A node that the host did not run for longer than the client's timeout: it carries out
    # the request when it runs again. Sending it again meanwhile would make it destroy ten
    # transmissions instead of five.
    def stalled(token, command):
        time.sleep(0.25)
        return "ok tx_errors=5"

    node = fake_node({"can.inject": stalled})
    with SimControl(node.port, timeout=0.1) as sim:
        assert sim.can_inject("tx", 5) == {"tx_errors": "5"}
    assert node.requests == ["can.inject tx 5"]


def test_repeatable_commands_are_the_ones_that_read_or_set_a_state():
    assert REPEATABLE == {"ping", "can.status", "can.fault", "can.trace", "sensor.status",
                          "sensor.set", "sensor.open", "sensor.offline", "sensor.hang",
                          "sensor.clear", "flash.status"}
    assert "flash.cut" not in REPEATABLE      # it counts from the moment it is served


# ------------------------------------------------------------------- sensor front-end model


def test_sensor_channels_are_the_current_eight_cells_and_four_temperatures():
    assert SENSOR_CHANNELS == ("current", "cell1", "cell2", "cell3", "cell4", "cell5", "cell6",
                               "cell7", "cell8", "temp1", "temp2", "temp3", "temp4")


def test_sensor_status_converts_numbers_negative_ones_included_and_keeps_words(fake_node):
    node = fake_node({"sensor.status": "ok seed=42 offline=on current_reads=120 cell_reads=60 "
                                       "temp_reads=12 unanswered=32 current=-7 cell1=model "
                                       "cell2=4300 temp1=-123 temp4=open"})
    with SimControl(node.port) as sim:
        assert sim.sensor_status() == {
            "seed": 42, "offline": "on", "current_reads": 120, "cell_reads": 60,
            "temp_reads": 12, "unanswered": 32, "current": -7, "cell1": "model", "cell2": 4300,
            "temp1": -123, "temp4": "open"}
    assert node.requests == ["sensor.status"]


def test_sensor_commands_carry_their_arguments(fake_node):
    node = fake_node({"sensor.set": "ok cell3=4300 t_ns=5", "sensor.open": "ok temp2=open t_ns=6",
                      "sensor.offline": "ok offline=on t_ns=7",
                      "sensor.clear": "ok cleared=all offline=off t_ns=8"})
    with SimControl(node.port) as sim:
        assert sim.sensor_set("cell3", 4300) == {"cell3": "4300", "t_ns": "5"}
        sim.sensor_set("current", -2147483648)
        assert sim.sensor_open("temp2") == {"temp2": "open", "t_ns": "6"}
        assert sim.sensor_offline() == {"offline": "on", "t_ns": "7"}
        sim.sensor_offline(True)
        sim.sensor_offline(False)
        assert sim.sensor_clear() == {"cleared": "all", "offline": "off", "t_ns": "8"}
        sim.sensor_clear("cell3")
    assert node.requests == ["sensor.set cell3 4300", "sensor.set current -2147483648",
                             "sensor.open temp2", "sensor.offline on", "sensor.offline on",
                             "sensor.offline off", "sensor.clear", "sensor.clear cell3"]


def test_sensor_hang_sends_its_mode_and_takes_true_and_false_for_on_and_off(fake_node):
    node = fake_node({"sensor.hang": lambda token, command:
                      f"ok hang={command.split(' ')[1]} t_ns=9"})
    with SimControl(node.port) as sim:
        assert sim.sensor_hang() == {"hang": "on", "t_ns": "9"}
        assert sim.sensor_hang("spin") == {"hang": "spin", "t_ns": "9"}
        assert sim.sensor_hang("off") == {"hang": "off", "t_ns": "9"}
        sim.sensor_hang(True)
        sim.sensor_hang(False)
    assert node.requests == ["sensor.hang on", "sensor.hang spin", "sensor.hang off",
                             "sensor.hang on", "sensor.hang off"]
    assert HANG_MODES == ("on", "spin", "off")


@pytest.mark.parametrize("mode", ["busy", "ON", "", None, 1, 0])
def test_sensor_hang_takes_only_its_modes_and_sends_nothing_otherwise(fake_node, mode):
    node = fake_node({"sensor.hang": "ok hang=on t_ns=1"})
    with SimControl(node.port) as sim:
        with pytest.raises(ValueError, match="a hang mode is one of on, spin, off"):
            sim.sensor_hang(mode)
    assert node.requests == []


def test_a_refused_hang_raises_with_the_nodes_message(fake_node):
    node = fake_node({"sensor.hang": "err usage: sensor.hang on|spin|off"})
    with SimControl(node.port) as sim:
        with pytest.raises(SimError, match=r"usage: sensor\.hang on\|spin\|off"):
            sim.sensor_hang("spin")
    assert node.requests == ["sensor.hang spin"]


@pytest.mark.parametrize("value", [3.7, "12", True, None])
def test_sensor_set_takes_only_a_whole_number_and_sends_nothing_otherwise(fake_node, value):
    node = fake_node({"sensor.set": "ok cell1=1 t_ns=1"})
    with SimControl(node.port) as sim:
        with pytest.raises(TypeError, match="whole number"):
            sim.sensor_set("cell1", value)
    assert node.requests == []


def test_a_refused_sensor_command_raises_with_the_nodes_message(fake_node):
    node = fake_node({"sensor.set": "err unknown channel 'cell9' (current, cell1..cell8, "
                                    "temp1..temp4)"})
    with SimControl(node.port) as sim:
        with pytest.raises(SimError, match="unknown channel 'cell9'"):
            sim.sensor_set("cell9", 1)
    assert node.requests == ["sensor.set cell9 1"]   # an answer: not asked again


@pytest.mark.parametrize("call, command", [
    (lambda sim: sim.sensor_status(), "sensor.status"),
    (lambda sim: sim.sensor_set("cell1", 5), "sensor.set cell1 5"),
    (lambda sim: sim.sensor_open("cell1"), "sensor.open cell1"),
    (lambda sim: sim.sensor_offline(True), "sensor.offline on"),
    (lambda sim: sim.sensor_hang("spin"), "sensor.hang spin"),
    (lambda sim: sim.sensor_clear(), "sensor.clear"),
])
def test_a_sensor_command_is_asked_again_when_the_first_attempt_goes_unanswered(fake_node, call,
                                                                               command):
    attempts = []

    def second_time(token, request):
        attempts.append(token)
        return "ok seed=1" if len(attempts) >= 2 else None

    node = fake_node({command.split(" ")[0]: second_time})
    with SimControl(node.port, timeout=0.1) as sim:
        call(sim)
    assert node.requests == [command, command]


# ----------------------------------------------------------------------- the simulated flash

FLASH_STATUS = ("ok flash=on size=2105344 reads=12 bytes_read=3072 programs=7 units_programmed=40 "
                "erases=3 refused=1 busy_ns=137800000 erases_meta=1 erases_slot_a=2 "
                "erases_slot_b=0 max_sector_erases=2 cut=armed cut_in=4 cut_seed=4242")


def test_flash_status_converts_numbers_and_keeps_words(fake_node):
    node = fake_node({"flash.status": FLASH_STATUS})
    with SimControl(node.port) as sim:
        assert sim.flash_status() == {
            "flash": "on", "size": 2105344, "reads": 12, "bytes_read": 3072, "programs": 7,
            "units_programmed": 40, "erases": 3, "refused": 1, "busy_ns": 137800000,
            "erases_meta": 1, "erases_slot_a": 2, "erases_slot_b": 0, "max_sector_erases": 2,
            "cut": "armed", "cut_in": 4, "cut_seed": 4242}
    assert node.requests == ["flash.status"]


def test_flash_status_of_a_node_without_a_flash_is_just_off(fake_node):
    node = fake_node({"flash.status": "ok flash=off"})
    with SimControl(node.port) as sim:
        assert sim.flash_status() == {"flash": "off"}


def test_flash_cut_sends_n_colon_seed_and_off_disarms(fake_node):
    node = fake_node({"flash.cut": lambda token, command:
                      "ok cut=none t_ns=3" if command.endswith(" off") else
                      f"ok cut=armed cut_in={command.split(' ')[1].split(':')[0]} "
                      f"cut_seed={command.split(':')[1]} t_ns=2"})
    with SimControl(node.port) as sim:
        assert sim.flash_cut(3, 42) == {"cut": "armed", "cut_in": "3", "cut_seed": "42",
                                        "t_ns": "2"}
        assert sim.flash_cut(1) == {"cut": "armed", "cut_in": "1", "cut_seed": "0", "t_ns": "2"}
        sim.flash_cut(FLASH_CUT_MAX, FLASH_SEED_MAX)
        assert sim.flash_cut_off() == {"cut": "none", "t_ns": "3"}
    assert node.requests == ["flash.cut 3:42", "flash.cut 1:0",
                             "flash.cut 2147483647:4294967295", "flash.cut off"]
    assert (FLASH_CUT_MAX, FLASH_SEED_MAX) == (2**31 - 1, 2**32 - 1)


@pytest.mark.parametrize("operation, seed, error", [
    (0, 0, ValueError), (-1, 0, ValueError), (FLASH_CUT_MAX + 1, 0, ValueError),
    (1, -1, ValueError), (1, FLASH_SEED_MAX + 1, ValueError),
    (1.0, 0, TypeError), ("1", 0, TypeError), (True, 0, TypeError), (None, 0, TypeError),
    (1, 2.5, TypeError), (1, "7", TypeError), (1, False, TypeError),
])
def test_flash_cut_takes_only_whole_numbers_in_range_and_sends_nothing_otherwise(
        fake_node, operation, seed, error):
    node = fake_node({"flash.cut": "ok cut=armed cut_in=1 cut_seed=0 t_ns=1"})
    with SimControl(node.port) as sim:
        with pytest.raises(error, match="a flash cut's"):
            sim.flash_cut(operation, seed)
    assert node.requests == []


def test_a_flash_cut_is_sent_once_and_waited_for_even_without_an_answer(fake_node):
    # Delivered a second time after an operation, the cut would count from a later moment.
    node = fake_node({"flash.cut": lambda token, command: None})
    started = time.monotonic()
    with SimControl(node.port, timeout=0.1) as sim:
        with pytest.raises(SimError, match="no reply to 'flash.cut 5:1'"):
            sim.request("flash.cut 5:1", retries=2)
    assert time.monotonic() - started >= 0.3
    assert node.requests == ["flash.cut 5:1"]


def test_a_flash_status_is_asked_again_when_the_first_attempt_goes_unanswered(fake_node):
    attempts = []

    def second_time(token, request):
        attempts.append(token)
        return "ok flash=off" if len(attempts) >= 2 else None

    node = fake_node({"flash.status": second_time})
    with SimControl(node.port, timeout=0.1) as sim:
        assert sim.flash_status() == {"flash": "off"}
    assert node.requests == ["flash.status", "flash.status"]


def test_a_refused_flash_cut_raises_with_the_nodes_message(fake_node):
    node = fake_node({"flash.cut": "err no flash in this run (started without --flash-file)"})
    with SimControl(node.port) as sim:
        with pytest.raises(SimError, match=r"no flash in this run \(started without "
                                           r"--flash-file\)"):
            sim.flash_cut(1)
    assert node.requests == ["flash.cut 1:0"]


def test_a_node_that_is_still_starting_is_asked_until_its_socket_is_open(fake_node):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((LOOPBACK, 0))
        port = probe.getsockname()[1]
    # Nothing listens on the port for the first 0.3 s, longer than one timeout: every
    # request until then is refused by the kernel, and only those are sent again, even for a
    # command that must not run twice.
    opened: list[FakeNode] = []
    opener = threading.Timer(
        0.3, lambda: opened.append(fake_node({"can.inject": "ok tx_errors=1"}, port)))
    opener.start()
    try:
        with SimControl(port, timeout=0.2) as sim:
            assert sim.can_inject("tx", 1) == {"tx_errors": "1"}
    finally:
        opener.join()
    assert opened[0].requests == ["can.inject tx 1"]


def test_a_node_that_answers_the_second_time_is_enough(fake_node):
    attempts = []

    def second_time(token, command):
        attempts.append(token)
        return "ok pid=7" if len(attempts) >= 2 else None

    node = fake_node({"ping": second_time})
    with SimControl(node.port, timeout=0.1) as sim:
        assert sim.ping() == 7
    assert len(attempts) == 2 and attempts[0] != attempts[1]     # a fresh token per attempt


def test_nothing_listening_is_reported_not_raised_as_a_socket_error():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((LOOPBACK, 0))
        free_port = probe.getsockname()[1]
    with SimControl(free_port, timeout=0.1) as sim:
        with pytest.raises(SimError, match="no reply"):
            sim.request("ping", retries=1)
