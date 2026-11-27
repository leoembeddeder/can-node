# SPDX-License-Identifier: MIT
"""Schedulability of a task table under fixed-priority preemptive scheduling, as FreeRTOS does it.

    python -m fcn_tools.sched <table.json> [--json out.json]

reads a task table (schema "fcn.task_table/1", see parse_table()), prints a report and exits
with 0 if every task meets its deadline, 1 if one does not, and 2 for an input it refuses. The
report holds:

  - the total utilisation U = sum of C_i / T_i, with two sufficient tests: the bound of Liu and
    Layland (1973), U <= n(2^(1/n) - 1), and the hyperbolic bound of Bini, Buttazzo and
    Buttazzo (2003), prod(C_i / T_i + 1) <= 2. A sufficient test that holds proves the set
    schedulable; one that fails proves nothing. Both were derived for rate-monotonic
    priorities, deadlines equal to periods, independent tasks and releases without jitter;
    where the table breaks one of these assumptions the test is reported as not applicable
    and is not evaluated;
  - exact response-time analysis for fixed priorities (Joseph and Pandya 1986; Audsley, Burns,
    Richardson, Tindell and Wellings 1993) with blocking under priority inheritance and with
    release jitter. It is the verdict of the report and of the exit status;
  - whether the priorities are in rate-monotonic and in deadline-monotonic order and, when they
    are not in deadline-monotonic order, the analysis once more with that order.

What the analysis assumes about the system it is given:

  - one processor; the highest-priority ready job runs and preempts any other at once
    (configUSE_PREEMPTION). Tasks of equal priority share the processor in an order the
    analysis does not know (FreeRTOS time-slices them), so each one counts the others as
    interference, as if they had a higher priority;
  - task i's jobs arrive at least T_i apart (a periodic task: exactly T_i, the period of its
    xTaskDelayUntil loop; a sporadic task: its minimum inter-arrival time), and a job becomes
    ready at most J_i (release jitter) after it arrives. Deadlines and response times count
    from the arrival;
  - a job executes for at most C_i, its own critical sections, kernel calls and context
    switches included, and does not suspend itself before it completes, except to wait for a
    mutex that another task holds;
  - the shared resources are mutexes with priority inheritance (FreeRTOS mutexes), and no
    critical section on a mutex is nested inside another. Besides those, a job may run
    non-preemptively for a while (taskENTER_CRITICAL, vTaskSuspendAll), at most for the
    longest such stretch the table gives;
  - interrupt handlers and the kernel's tick exist for the analysis only as far as they are
    tasks of the table (sporadic tasks above every other priority).

All times are integers in microseconds. All arithmetic is exact, in integers and fractions:
a response time equal to a deadline, or a product of the hyperbolic bound equal to 2, is on the
right side of its comparison, which binary floating point cannot promise.

Response times (Lehoczky 1990; Tindell, Burns and Wellings 1994). Time 0 is the critical
instant: every task of priority at least i's releases a job at 0 after its longest possible
jitter and every later job as early as it can, and the lower-priority tasks that can block i
hold their resources. Job q (q = 0, 1, ...) of task i in the busy period that starts there
completes at w_i(q), the smallest solution of

    w = B_i + (q + 1) C_i + sum over j in hp(i) of ceil((w + J_j) / T_j) C_j

where hp(i) holds the tasks of higher priority and the others of equal priority. It arrived
at q T_i - J_i, so its response time is R_i(q) = w_i(q) - q T_i + J_i. The busy period ends
with the first job that completes before the next one is released, w_i(q) <= (q + 1) T_i - J_i,
and R_i is the largest R_i(q) up to that job. With deadlines no longer than periods, a task
that meets its deadline has a single job in its busy period: R_i(0) <= D_i <= T_i satisfies the
end condition at q = 0, and w_i(0) is r_i of equation (5) of Audsley et al. (1993), R_i(0) =
J_i + r_i their equation (6). The later jobs only make the response time reported for a task
that misses its deadline the worst one, rather than the first job's.

When the tasks at or above i's priority need more than the whole processor, R_i has no bound.
At exactly the whole processor the busy period ends (at the latest after one hyperperiod) only
if there is no blocking and no jitter at that level; with either, it never ends and the
analysis reports no bound. Every task without a bound counts as missing its deadline.

Blocking (Sha, Rajkumar and Lehoczky 1990). A lower-priority task delays task i while it holds
a mutex that i takes (direct blocking) or that a task of priority at least i's takes, whose
priority the holder then inherits (push-through blocking). Under priority inheritance a job is
blocked at most once on each such mutex and at most once by each lower-priority task, each
time for at most one critical section. B_i is the smaller of the two sums this allows: over
those mutexes, the longest critical section a lower-priority task holds on it; over the
lower-priority tasks, the longest critical section each holds on any of them. A non-preemptive
stretch of a lower-priority task blocks every task above it, as a mutex would that every task
takes: it counts as one more such resource. B_i counts once per busy period, not once per job:
a lower-priority task can only block with a section it was already in when the busy period
started, because once it has left it, it runs at its own priority, which gets no processor time
until the busy period ends.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Any

TABLE_SCHEMA = "fcn.task_table/1"
REPORT_SCHEMA = "fcn.sched_report/1"

EXIT_SCHEDULABLE = 0
EXIT_NOT_SCHEDULABLE = 1
EXIT_BAD_INPUT = 2

PERIODIC = "periodic"
SPORADIC = "sporadic"

#: What the execution times of a table are, when the table says: budgets from the design, or
#: execution times measured on the node.
WCET_BASES = ("design", "measured")

# The field that holds T_i, by kind of task: the same quantity under the name it has.
_PERIOD_FIELD = {PERIODIC: "period_us", SPORADIC: "min_interarrival_us"}
_TASK_FIELDS = ("name", "priority", "kind", "deadline_us", "wcet_us", "jitter_us",
                "nonpreemptive_us", "critical_sections")

#: The resource a Blocker names when the section that blocks is a non-preemptive stretch of the
#: lower-priority task rather than a mutex. No mutex may have this name.
NONPREEMPTIVE = "non-preemptive"
_SECTION_FIELDS = ("resource", "length_us")
_TABLE_REQUIRED = ("schema", "tasks")
_TABLE_OPTIONAL = ("wcet_basis", "description")

LIU_LAYLAND = "liu_layland"
HYPERBOLIC = "hyperbolic"


class TaskTableError(ValueError):
    """The input is not a valid fcn.task_table/1 document; the message says what is wrong."""


def _check_int(value: Any, what: str, minimum: int) -> None:
    # bool is an int in Python; true is not a priority.
    if isinstance(value, bool) or not isinstance(value, int):
        raise TaskTableError(f"{what} must be an integer, not {value!r}")
    if value < minimum:
        raise TaskTableError(f"{what} must be at least {minimum}, not {value}")


def _check_name(value: Any, what: str) -> None:
    if not isinstance(value, str) or not value:
        raise TaskTableError(f"{what} must be a non-empty string, not {value!r}")


@dataclass(frozen=True)
class CriticalSection:
    """The longest critical section a task holds on one mutex, in microseconds."""

    resource: str
    length_us: int

    def __post_init__(self) -> None:
        _check_name(self.resource, "resource")
        if self.resource == NONPREEMPTIVE:
            raise TaskTableError(f"resource {NONPREEMPTIVE!r} is reserved for non-preemptive "
                                 "sections (nonpreemptive_us)")
        _check_int(self.length_us, f"length_us of the section on {self.resource!r}", 1)


@dataclass(frozen=True)
class Task:
    """One task of the table. Every instance is valid: the constructor checks every rule.

    `period_us` is T_i: the period of a periodic task, the minimum inter-arrival time of a
    sporadic one. `priority` is the FreeRTOS priority: a higher number is more urgent.
    `nonpreemptive_us` is the longest stretch of one job that no other task can preempt
    (inside taskENTER_CRITICAL/taskEXIT_CRITICAL or vTaskSuspendAll/xTaskResumeAll; the
    outermost region if they nest), 0 if there is none.
    """

    name: str
    priority: int
    kind: str
    period_us: int
    deadline_us: int
    wcet_us: int
    jitter_us: int = 0
    critical_sections: tuple[CriticalSection, ...] = ()
    nonpreemptive_us: int = 0

    def __post_init__(self) -> None:
        _check_name(self.name, "name")
        what = f"task {self.name!r}"
        _check_int(self.priority, f"{what}: priority", 0)
        if self.kind not in _PERIOD_FIELD:
            raise TaskTableError(f"{what}: kind must be {PERIODIC!r} or {SPORADIC!r}, "
                                 f"not {self.kind!r}")
        period_field = _PERIOD_FIELD[self.kind]
        _check_int(self.period_us, f"{what}: {period_field}", 1)
        _check_int(self.deadline_us, f"{what}: deadline_us", 1)
        if self.deadline_us > self.period_us:
            raise TaskTableError(f"{what}: deadline_us {self.deadline_us} is longer than "
                                 f"{period_field} {self.period_us} (the analysis requires D <= T)")
        _check_int(self.wcet_us, f"{what}: wcet_us", 1)
        _check_int(self.jitter_us, f"{what}: jitter_us", 0)
        _check_int(self.nonpreemptive_us, f"{what}: nonpreemptive_us", 0)
        if self.nonpreemptive_us > self.wcet_us:
            raise TaskTableError(f"{what}: nonpreemptive_us {self.nonpreemptive_us} is longer "
                                 f"than wcet_us {self.wcet_us}")
        if not isinstance(self.critical_sections, tuple) or not all(
                isinstance(section, CriticalSection) for section in self.critical_sections):
            raise TaskTableError(f"{what}: critical_sections must be CriticalSection objects")
        seen: set[str] = set()
        for section in self.critical_sections:
            if section.resource in seen:
                raise TaskTableError(f"{what}: resource {section.resource!r} is listed twice "
                                     "(give its longest critical section once)")
            seen.add(section.resource)
            if section.length_us > self.wcet_us:
                raise TaskTableError(f"{what}: the section on {section.resource!r} "
                                     f"({section.length_us}) is longer than wcet_us "
                                     f"{self.wcet_us}")
        total = sum(section.length_us for section in self.critical_sections)
        if total > self.wcet_us:
            # Sections are not nested, so they are disjoint parts of one job's execution.
            raise TaskTableError(f"{what}: its critical sections add up to {total}, more than "
                                 f"wcet_us {self.wcet_us}")

    @property
    def resources(self) -> frozenset[str]:
        return frozenset(section.resource for section in self.critical_sections)

    def section_us(self, resource: str) -> int:
        """Length of the task's critical section on `resource`; 0 if it does not use it."""
        for section in self.critical_sections:
            if section.resource == resource:
                return section.length_us
        return 0

    @property
    def utilisation(self) -> Fraction:
        return Fraction(self.wcet_us, self.period_us)


@dataclass(frozen=True)
class TaskTable:
    """A task table: at least one task, names unique."""

    tasks: tuple[Task, ...]
    wcet_basis: str | None = None
    description: str | None = None

    def __post_init__(self) -> None:
        if not self.tasks:
            raise TaskTableError("the table has no task")
        names = [task.name for task in self.tasks]
        duplicates = sorted({name for name in names if names.count(name) > 1})
        if duplicates:
            raise TaskTableError(f"task names must be unique: {', '.join(duplicates)}")
        if self.wcet_basis is not None and self.wcet_basis not in WCET_BASES:
            raise TaskTableError(f"wcet_basis must be one of {', '.join(WCET_BASES)}, "
                                 f"not {self.wcet_basis!r}")
        if self.description is not None and not isinstance(self.description, str):
            raise TaskTableError(f"description must be a string, not {self.description!r}")


# ------------------------------------------------------------------------------- reading


def _object_without_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise TaskTableError(f"key {key!r} appears twice in one object")
        result[key] = value
    return result


def _no_constant(name: str) -> Any:
    raise TaskTableError(f"{name} is not a number")


def _check_keys(obj: Any, what: str, required: Iterable[str],
                optional: Iterable[str] = ()) -> None:
    if not isinstance(obj, dict):
        raise TaskTableError(f"{what} must be an object, not {obj!r}")
    required = tuple(required)
    allowed = set(required) | set(optional)
    missing = [key for key in required if key not in obj]
    if missing:
        raise TaskTableError(f"{what}: missing {', '.join(missing)}")
    unknown = sorted(key for key in obj if key not in allowed)
    if unknown:
        raise TaskTableError(f"{what}: unknown field(s) {', '.join(unknown)}")


def _task_from_json(obj: Any, index: int) -> Task:
    what = f"tasks[{index}]"
    if not isinstance(obj, dict):
        raise TaskTableError(f"{what} must be an object, not {obj!r}")
    if isinstance(obj.get("name"), str):
        what += f" ({obj['name']})"
    # The kind decides which field holds T_i, so it is checked before the other fields.
    if "kind" not in obj:
        raise TaskTableError(f"{what}: missing kind")
    kind = obj["kind"]
    if not isinstance(kind, str) or kind not in _PERIOD_FIELD:
        raise TaskTableError(f"{what}: kind must be {PERIODIC!r} or {SPORADIC!r}, not {kind!r}")
    period_field = _PERIOD_FIELD[kind]
    _check_keys(obj, what, (*_TASK_FIELDS, period_field))
    sections = obj["critical_sections"]
    if not isinstance(sections, list):
        raise TaskTableError(f"{what}: critical_sections must be a list, not {sections!r}")
    parsed = []
    for number, section in enumerate(sections):
        _check_keys(section, f"{what}: critical_sections[{number}]", _SECTION_FIELDS)
        try:
            parsed.append(CriticalSection(section["resource"], section["length_us"]))
        except TaskTableError as error:
            raise TaskTableError(f"{what}: critical_sections[{number}]: {error}") from None
    try:
        return Task(name=obj["name"], priority=obj["priority"], kind=kind,
                    period_us=obj[period_field], deadline_us=obj["deadline_us"],
                    wcet_us=obj["wcet_us"], jitter_us=obj["jitter_us"],
                    critical_sections=tuple(parsed), nonpreemptive_us=obj["nonpreemptive_us"])
    except TaskTableError as error:
        raise TaskTableError(f"{what}: {error}") from None


def parse_table(text: str) -> TaskTable:
    """A task table from its JSON text. Raises TaskTableError for anything else.

    The document is an object::

        {"schema": "fcn.task_table/1",
         "wcet_basis": "design",                 optional: "design" or "measured"
         "description": "...",                   optional
         "tasks": [
           {"name": "sensor", "priority": 5, "kind": "periodic", "period_us": 10000,
            "deadline_us": 10000, "wcet_us": 400, "jitter_us": 0, "nonpreemptive_us": 0,
            "critical_sections": [{"resource": "pack_state", "length_us": 20}]},
           {"name": "can_rx", "priority": 6, "kind": "sporadic", "min_interarrival_us": 500,
            "deadline_us": 500, "wcet_us": 40, "jitter_us": 0, "nonpreemptive_us": 5,
            "critical_sections": []},
           ...]}

    (The numbers are an illustration of the format, not figures of the node.) Every field of
    a task is required, and a sporadic task gives its minimum inter-arrival time as
    "min_interarrival_us" instead of "period_us". Times are integers in microseconds: period
    and deadline at least 1 with deadline <= period, wcet at least 1, jitter at least 0,
    nonpreemptive_us (the longest stretch of a job that no other task can preempt, see Task)
    from 0 to wcet. A critical section names a mutex and the longest time the task holds it
    in one job (at least 1); each mutex appears at most once per task, and a task's sections
    together are no longer than its wcet (they are not nested, so they are separate parts of
    one job). Unknown fields, duplicate keys, numbers that are not integers and NaN or
    Infinity are refused, so that a misspelt field cannot silently fall back to a default.
    """
    try:
        document = json.loads(text, object_pairs_hook=_object_without_duplicate_keys,
                              parse_constant=_no_constant)
    except json.JSONDecodeError as error:
        raise TaskTableError(f"not valid JSON: {error}") from None
    except RecursionError:
        raise TaskTableError("not valid JSON: nested too deeply") from None
    _check_keys(document, "the table", _TABLE_REQUIRED, _TABLE_OPTIONAL)
    if document["schema"] != TABLE_SCHEMA:
        raise TaskTableError(f"schema must be {TABLE_SCHEMA!r}, not {document['schema']!r}")
    tasks = document["tasks"]
    if not isinstance(tasks, list):
        raise TaskTableError(f"tasks must be a list, not {tasks!r}")
    return TaskTable(tasks=tuple(_task_from_json(obj, index) for index, obj in enumerate(tasks)),
                     wcet_basis=document.get("wcet_basis"),
                     description=document.get("description"))


def load_table(path: str | Path) -> TaskTable:
    """parse_table() of a file; a file that cannot be read is a TaskTableError too."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise TaskTableError(f"cannot read the table: {error}") from None
    return parse_table(text)


# ---------------------------------------------------------------------------- the tests


def utilisation(tasks: Iterable[Task]) -> Fraction:
    """U = sum of C_i / T_i, exactly."""
    return sum((task.utilisation for task in tasks), Fraction(0))


def liu_layland_limit(n: int) -> float:
    """n(2^(1/n) - 1), for display: liu_layland_holds() compares without rounding."""
    return n * (2.0 ** (1.0 / n) - 1.0)


def liu_layland_holds(total: Fraction, n: int) -> bool:
    """U <= n(2^(1/n) - 1), decided exactly: for U >= 0 it is (1 + U/n)^n <= 2."""
    return (1 + total / n) ** n <= 2


def hyperbolic_product(tasks: Iterable[Task]) -> Fraction:
    """prod(C_i / T_i + 1), exactly; the hyperbolic bound holds when it is at most 2."""
    product = Fraction(1)
    for task in tasks:
        product *= task.utilisation + 1
    return product


def by_priority(tasks: Iterable[Task]) -> list[Task]:
    """Highest priority first; equal priorities in table order."""
    return sorted(tasks, key=lambda task: -task.priority)


def interfering(task: Task, tasks: Iterable[Task]) -> list[Task]:
    """hp(i): the tasks of higher priority and the other tasks of equal priority."""
    return [other for other in tasks if other.name != task.name and other.priority >= task.priority]


@dataclass(frozen=True)
class Blocker:
    """One critical section counted in a blocking bound: `task` holding `resource`."""

    resource: str
    task: str
    length_us: int


@dataclass(frozen=True)
class Blocking:
    """B_i under priority inheritance and how it was found.

    `per_resource` holds, for every mutex that can block the task, the longest section a
    lower-priority task holds on it; `per_task` holds, for every lower-priority task that can
    block it, its longest such section. `blocking_us` is the smaller of the two sums.
    """

    blocking_us: int
    per_resource: tuple[Blocker, ...]
    per_task: tuple[Blocker, ...]

    @property
    def per_resource_us(self) -> int:
        return sum(blocker.length_us for blocker in self.per_resource)

    @property
    def per_task_us(self) -> int:
        return sum(blocker.length_us for blocker in self.per_task)


def _longest(candidates: list[Blocker]) -> Blocker:
    # The longest section; between equal lengths the first resource and task by name, so that
    # the report is the same on every run.
    return min(candidates, key=lambda blocker: (-blocker.length_us, blocker.resource, blocker.task))


def blocking(task: Task, tasks: Iterable[Task]) -> Blocking:
    """B_i of `task` in the set `tasks` (which contains it) under priority inheritance.

    A mutex can block the task when a task of lower priority uses it and a task of priority at
    least the task's own (the task itself included) uses it too. A non-preemptive stretch of a
    lower-priority task can always block it; it is counted as the resource NONPREEMPTIVE.
    """
    tasks = list(tasks)
    lower = [other for other in tasks if other.priority < task.priority]
    at_or_above = [other for other in tasks if other.priority >= task.priority]
    can_block = set().union(*(other.resources for other in at_or_above))
    sections = [Blocker(section.resource, other.name, section.length_us)
                for other in lower for section in other.critical_sections
                if section.resource in can_block]
    sections += [Blocker(NONPREEMPTIVE, other.name, other.nonpreemptive_us)
                 for other in lower if other.nonpreemptive_us > 0]
    per_resource = tuple(_longest([s for s in sections if s.resource == resource])
                         for resource in sorted({s.resource for s in sections}))
    per_task = tuple(_longest([s for s in sections if s.task == name])
                     for name in sorted({s.task for s in sections}))
    blocking_us = min(sum(b.length_us for b in per_resource), sum(b.length_us for b in per_task))
    return Blocking(blocking_us=blocking_us, per_resource=per_resource, per_task=per_task)


@dataclass(frozen=True)
class Job:
    """Job q of the busy period: it completes at `finish_us` after the critical instant and
    `response_us` after its arrival."""

    index: int
    finish_us: int
    response_us: int


@dataclass(frozen=True)
class ResponseTime:
    """The worst-case response time of one task, or why it has none.

    `response_us` is None when there is no bound; `unbounded_reason` then says why and `jobs`
    is empty. Otherwise `jobs` lists the jobs of the busy period in order and `worst_job` is
    the index of the first one with the largest response time.
    """

    response_us: int | None
    jobs: tuple[Job, ...]
    level_utilisation: Fraction
    unbounded_reason: str | None = None

    @property
    def worst_job(self) -> int | None:
        if self.response_us is None:
            return None
        return next(job.index for job in self.jobs if job.response_us == self.response_us)

    @property
    def busy_period_us(self) -> int | None:
        """Length of the busy period: the last job's finish."""
        return self.jobs[-1].finish_us if self.jobs else None


def _ceil_div(numerator: int, denominator: int) -> int:
    return -(-numerator // denominator)


def response_time(task: Task, higher: Sequence[Task], blocking_us: int) -> ResponseTime:
    """R_i of `task` preempted by the tasks `higher` (hp(i)) and blocked for `blocking_us`.

    The busy-period iteration of the module docstring. `higher` must not contain the task.
    """
    _check_int(blocking_us, "blocking_us", 0)
    if any(other.name == task.name for other in higher):
        raise ValueError(f"{task.name} cannot preempt itself")
    level = task.utilisation + utilisation(higher)
    if level > 1:
        return ResponseTime(None, (), level, f"the tasks at and above its priority need "
                                             f"{float(level):.1%} of the processor")
    jittered = task.jitter_us > 0 or any(other.jitter_us > 0 for other in higher)
    if level == 1 and (blocking_us > 0 or jittered):
        return ResponseTime(None, (), level, "the tasks at and above its priority need the "
                                             "whole processor and there is blocking or jitter: "
                                             "the busy period never ends")
    jobs: list[Job] = []
    # The least solution of each equation is approached from below. Every job of every task in
    # hp(i) released at the critical instant is part of job 0's window, so the first guess
    # counts each once; job q's window holds job q-1's and C_i more.
    finish = blocking_us + task.wcet_us + sum(other.wcet_us for other in higher)
    q = 0
    while True:
        own = blocking_us + (q + 1) * task.wcet_us
        while True:
            demand = own + sum(_ceil_div(finish + other.jitter_us, other.period_us) * other.wcet_us
                               for other in higher)
            if demand == finish:
                break
            finish = demand
        jobs.append(Job(q, finish, finish - q * task.period_us + task.jitter_us))
        if finish <= (q + 1) * task.period_us - task.jitter_us:
            break
        q += 1
        finish += task.wcet_us
    return ResponseTime(max(job.response_us for job in jobs), tuple(jobs), level)


@dataclass(frozen=True)
class TaskResult:
    """Blocking, response time and verdict of one task."""

    task: Task
    blocking: Blocking
    response: ResponseTime
    interfering: tuple[str, ...]

    @property
    def response_us(self) -> int | None:
        return self.response.response_us

    @property
    def meets_deadline(self) -> bool:
        return self.response_us is not None and self.response_us <= self.task.deadline_us

    @property
    def slack_us(self) -> int | None:
        return None if self.response_us is None else self.task.deadline_us - self.response_us


def analyse_response_times(tasks: Sequence[Task]) -> tuple[TaskResult, ...]:
    """The response-time analysis of every task, highest priority first."""
    results = []
    for task in by_priority(tasks):
        higher = interfering(task, tasks)
        bound = blocking(task, tasks)
        results.append(TaskResult(task=task, blocking=bound,
                                  response=response_time(task, higher, bound.blocking_us),
                                  interfering=tuple(other.name for other in higher)))
    return tuple(results)


@dataclass(frozen=True)
class OrderViolation:
    """`urgent` has the shorter period (or deadline) but not the higher priority of `other`."""

    urgent: str
    other: str
    urgent_value_us: int
    other_value_us: int
    urgent_priority: int
    other_priority: int


def order_violations(tasks: Sequence[Task], key: str) -> tuple[OrderViolation, ...]:
    """Pairs out of rate-monotonic (key "period_us") or deadline-monotonic ("deadline_us")
    order: a shorter period or deadline needs a strictly higher priority. Tasks with the same
    period or deadline may have any priorities."""
    violations = []
    for urgent in tasks:
        for other in tasks:
            if (getattr(urgent, key) < getattr(other, key)
                    and urgent.priority <= other.priority):
                violations.append(OrderViolation(urgent.name, other.name, getattr(urgent, key),
                                                 getattr(other, key), urgent.priority,
                                                 other.priority))
    return tuple(violations)


def deadline_monotonic(tasks: Sequence[Task]) -> tuple[Task, ...]:
    """The tasks with deadline-monotonic priorities: distinct priorities n, n-1, ..., 1 from the
    shortest deadline to the longest. Between equal deadlines the given priorities keep their
    order, and between equal priorities too the table order does."""
    ranked = sorted(enumerate(tasks), key=lambda item: (item[1].deadline_us, -item[1].priority,
                                                        item[0]))
    return tuple(dataclasses.replace(task, priority=len(tasks) - rank)
                 for rank, (_, task) in enumerate(ranked))


@dataclass(frozen=True)
class BoundTest:
    """One of the two utilisation tests: its value against its limit, and whether it applies."""

    name: str
    value: Fraction
    limit: float
    holds: bool
    not_applicable: tuple[str, ...]

    @property
    def applies(self) -> bool:
        return not self.not_applicable

    @property
    def verdict(self) -> str:
        if not self.applies:
            return "not applicable"
        return "schedulable" if self.holds else "inconclusive"


@dataclass(frozen=True)
class Analysis:
    """Everything the report says about one table."""

    table: TaskTable
    utilisation: Fraction
    liu_layland: BoundTest
    hyperbolic: BoundTest
    rate_monotonic_violations: tuple[OrderViolation, ...]
    deadline_monotonic_violations: tuple[OrderViolation, ...]
    results: tuple[TaskResult, ...]
    deadline_monotonic_results: tuple[TaskResult, ...] | None

    @property
    def schedulable(self) -> bool:
        return all(result.meets_deadline for result in self.results)

    @property
    def rate_monotonic(self) -> bool:
        return not self.rate_monotonic_violations

    @property
    def deadline_monotonic(self) -> bool:
        return not self.deadline_monotonic_violations

    def result(self, name: str) -> TaskResult:
        return next(result for result in self.results if result.task.name == name)


def _names(tasks: Iterable[Task]) -> str:
    return ", ".join(task.name for task in tasks)


def _bound_assumptions(tasks: Sequence[Task], results: Sequence[TaskResult],
                       rm_violations: Sequence[OrderViolation]) -> tuple[str, ...]:
    """The assumptions of both utilisation tests that the table breaks, in words."""
    reasons = []
    short = [task for task in tasks if task.deadline_us < task.period_us]
    if short:
        reasons.append(f"deadline shorter than period: {_names(short)}")
    jittered = [task for task in tasks if task.jitter_us > 0]
    if jittered:
        reasons.append(f"release jitter: {_names(jittered)}")
    blocked = [result.task for result in results if result.blocking.blocking_us > 0]
    if blocked:
        reasons.append(f"blocking by lower-priority tasks: {_names(blocked)}")
    priorities = [task.priority for task in tasks]
    tied = [task for task in tasks if priorities.count(task.priority) > 1]
    if tied:
        reasons.append(f"equal priorities (time-sliced): {_names(tied)}")
    if rm_violations:
        reasons.append("priorities not in rate-monotonic order")
    return tuple(reasons)


def analyse(table: TaskTable) -> Analysis:
    """All tests of the report on one table."""
    tasks = table.tasks
    n = len(tasks)
    results = analyse_response_times(tasks)
    rm_violations = order_violations(tasks, "period_us")
    dm_violations = order_violations(tasks, "deadline_us")
    reasons = _bound_assumptions(tasks, results, rm_violations)
    total = utilisation(tasks)
    product = hyperbolic_product(tasks)
    return Analysis(
        table=table,
        utilisation=total,
        liu_layland=BoundTest(LIU_LAYLAND, total, liu_layland_limit(n),
                              liu_layland_holds(total, n), reasons),
        hyperbolic=BoundTest(HYPERBOLIC, product, 2.0, product <= 2, reasons),
        rate_monotonic_violations=rm_violations,
        deadline_monotonic_violations=dm_violations,
        results=results,
        deadline_monotonic_results=(analyse_response_times(deadline_monotonic(tasks))
                                    if dm_violations else None),
    )


# ------------------------------------------------------------------------------ reports


def _result_json(result: TaskResult) -> dict[str, Any]:
    task, response = result.task, result.response
    return {
        "name": task.name,
        "priority": task.priority,
        "kind": task.kind,
        "period_us": task.period_us,
        "deadline_us": task.deadline_us,
        "wcet_us": task.wcet_us,
        "jitter_us": task.jitter_us,
        "nonpreemptive_us": task.nonpreemptive_us,
        "interfering": list(result.interfering),
        "blocking_us": result.blocking.blocking_us,
        "blocking_per_resource_us": result.blocking.per_resource_us,
        "blocking_per_task_us": result.blocking.per_task_us,
        "blockers_per_resource": [dataclasses.asdict(b) for b in result.blocking.per_resource],
        "blockers_per_task": [dataclasses.asdict(b) for b in result.blocking.per_task],
        "level_utilisation": round(float(response.level_utilisation), 6),
        "response_us": result.response_us,
        "unbounded_reason": response.unbounded_reason,
        "worst_job": response.worst_job,
        "jobs_in_busy_period": len(response.jobs),
        "busy_period_us": response.busy_period_us,
        "meets_deadline": result.meets_deadline,
        "slack_us": result.slack_us,
    }


def _bound_json(bound: BoundTest) -> dict[str, Any]:
    return {"value": round(float(bound.value), 6), "limit": round(bound.limit, 6),
            "holds": bound.holds, "applies": bound.applies,
            "not_applicable_because": list(bound.not_applicable), "verdict": bound.verdict}


def _violation_json(violation: OrderViolation) -> dict[str, Any]:
    return dataclasses.asdict(violation)


def report_json(analysis: Analysis, source: str) -> dict[str, Any]:
    """The report as a JSON-serialisable object (schema fcn.sched_report/1). Floats are
    rounded to six places for reading; every verdict was decided exactly."""
    alternative = None
    if analysis.deadline_monotonic_results is not None:
        results = analysis.deadline_monotonic_results
        alternative = {
            "order": [result.task.name for result in results],
            "schedulable": all(result.meets_deadline for result in results),
            "tasks": [_result_json(result) for result in results],
        }
    return {
        "schema": REPORT_SCHEMA,
        "table": source,
        "wcet_basis": analysis.table.wcet_basis,
        "description": analysis.table.description,
        "schedulable": analysis.schedulable,
        "utilisation": round(float(analysis.utilisation), 6),
        "utilisation_at_most_1": analysis.utilisation <= 1,
        "liu_layland": _bound_json(analysis.liu_layland),
        "hyperbolic": _bound_json(analysis.hyperbolic),
        "rate_monotonic": analysis.rate_monotonic,
        "rate_monotonic_violations": [_violation_json(v)
                                      for v in analysis.rate_monotonic_violations],
        "deadline_monotonic": analysis.deadline_monotonic,
        "deadline_monotonic_violations": [_violation_json(v)
                                          for v in analysis.deadline_monotonic_violations],
        "tasks": [_result_json(result) for result in analysis.results],
        "with_deadline_monotonic_priorities": alternative,
    }


_BASIS_TEXT = {
    "design": "design budgets, not measurements",
    "measured": "measured on the node",
    None: "the table does not say whether they are budgets or measurements",
}


def _order_text(violations: Sequence[OrderViolation], quantity: str) -> str:
    if not violations:
        return "yes"
    first = violations[0]
    more = f" (and {len(violations) - 1} more pair(s))" if len(violations) > 1 else ""
    return (f"no: {first.urgent} ({quantity} {first.urgent_value_us}, priority "
            f"{first.urgent_priority}) is not above {first.other} ({quantity} "
            f"{first.other_value_us}, priority {first.other_priority}){more}")


def _bound_line(label: str, formula: str, bound: BoundTest) -> str:
    if not bound.applies:
        state = "not applicable: " + "; ".join(bound.not_applicable)
    elif bound.holds:
        state = "holds: schedulable"
    else:
        state = "fails: inconclusive (a sufficient test only)"
    return f"  {label:<18} {formula:<44} {state}"


def _table_lines(results: Sequence[TaskResult]) -> list[str]:
    header = ("task", "prio", "kind", "T", "D", "C", "J", "B", "R", "slack", "job", "verdict")
    rows = [header]
    for result in results:
        task, response = result.task, result.response
        if result.response_us is None:
            r_text, slack, job, verdict = "none", "-", "-", "MISSES (no bound)"
        else:
            r_text, slack = str(result.response_us), str(result.slack_us)
            job = f"{response.worst_job + 1}/{len(response.jobs)}"
            verdict = "meets" if result.meets_deadline else "MISSES"
        rows.append((task.name, str(task.priority), task.kind, str(task.period_us),
                     str(task.deadline_us), str(task.wcet_us), str(task.jitter_us),
                     str(result.blocking.blocking_us), r_text, slack, job, verdict))
    widths = [max(len(row[column]) for row in rows) for column in range(len(header))]
    lines = []
    for row in rows:
        cells = [row[0].ljust(widths[0]), row[1].rjust(widths[1]), row[2].ljust(widths[2])]
        cells += [cell.rjust(width) for cell, width in zip(row[3:11], widths[3:11])]
        cells.append(row[11])
        lines.append("  " + "  ".join(cells).rstrip())
    return lines


def _section_text(blocker: Blocker, task_first: bool) -> str:
    """A blocker in words: 'r held by low 3' per resource, 'low on r 3' per task."""
    if blocker.resource == NONPREEMPTIVE:
        return f"{blocker.task} non-preemptive {blocker.length_us}"
    if task_first:
        return f"{blocker.task} on {blocker.resource} {blocker.length_us}"
    return f"{blocker.resource} held by {blocker.task} {blocker.length_us}"


def _blocking_lines(results: Sequence[TaskResult]) -> list[str]:
    lines = []
    for result in results:
        bound = result.blocking
        if bound.blocking_us == 0:
            continue
        by_resource = ", ".join(_section_text(b, task_first=False) for b in bound.per_resource)
        text = f"  {result.task.name}: B = {bound.blocking_us}, once per resource: {by_resource}"
        if bound.per_task_us < bound.per_resource_us:
            by_task = ", ".join(_section_text(b, task_first=True) for b in bound.per_task)
            text = (f"  {result.task.name}: B = {bound.blocking_us}, once per lower-priority "
                    f"task: {by_task} (once per resource would be {bound.per_resource_us})")
        lines.append(text)
    return lines


def _verdict_text(results: Sequence[TaskResult]) -> str:
    misses = [result for result in results if not result.meets_deadline]
    if not misses:
        return "schedulable: every task meets its deadline"
    parts = []
    for result in misses:
        if result.response_us is None:
            parts.append(f"{result.task.name} has no bound ({result.response.unbounded_reason})")
        else:
            parts.append(f"{result.task.name} misses (R {result.response_us} > D "
                         f"{result.task.deadline_us})")
    return "NOT schedulable: " + "; ".join(parts)


def format_report(analysis: Analysis, source: str) -> str:
    """The report as text, as the command prints it."""
    tasks = analysis.table.tasks
    n = len(tasks)
    lines = [
        f"Task table {source} ({TABLE_SCHEMA}, {n} task{'s' if n != 1 else ''})",
        f"Execution times: {_BASIS_TEXT[analysis.table.wcet_basis]}. All times in microseconds.",
    ]
    if analysis.table.description:
        lines.append(f"Description: {analysis.table.description}")
    total = analysis.utilisation
    lines += [
        "",
        f"Utilisation U = sum of C/T = {float(total):.4f}",
        f"  {'necessary':<18} {'U <= 1':<44} "
        + ("holds" if total <= 1 else "fails: NOT schedulable with these execution times"),
        _bound_line("Liu and Layland", f"U <= n(2^(1/n) - 1) = {analysis.liu_layland.limit:.4f}"
                    f" for n = {n}", analysis.liu_layland),
        _bound_line("hyperbolic", f"prod(C/T + 1) = {float(analysis.hyperbolic.value):.4f} <= 2",
                    analysis.hyperbolic),
        "",
        "Priority order (a higher number is more urgent)",
        f"  rate-monotonic      {_order_text(analysis.rate_monotonic_violations, 'T')}",
        f"  deadline-monotonic  {_order_text(analysis.deadline_monotonic_violations, 'D')}",
        "",
        "Response-time analysis: fixed priorities, preemptive, priority inheritance, release "
        "jitter",
        "(T period or minimum inter-arrival time, D deadline, C execution time, J release "
        "jitter, B blocking, R worst-case response time from arrival; job k/n: the k-th of the "
        "n jobs of the busy period has the worst response)",
        *_table_lines(analysis.results),
    ]
    blocking_lines = _blocking_lines(analysis.results)
    if blocking_lines:
        lines += ["Blocking by lower-priority tasks (priority inheritance):", *blocking_lines]
    tied = [task for task in tasks if [t.priority for t in tasks].count(task.priority) > 1]
    if tied:
        lines.append(f"Tasks of equal priority count each other as interference: {_names(tied)}")
    lines += ["", f"Verdict: {_verdict_text(analysis.results)}"]
    if analysis.deadline_monotonic_results is not None:
        results = analysis.deadline_monotonic_results
        lines += [
            "",
            "The priorities are not in deadline-monotonic order. With deadline-monotonic "
            "priorities (optimal without blocking and jitter, Leung and Whitehead 1982; with "
            "them, another order can still do better), highest first:",
            "  " + " > ".join(result.task.name for result in results),
            *_table_lines(results),
            f"  {_verdict_text(results)}",
        ]
    return "\n".join(lines) + "\n"


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m fcn_tools.sched",
        description="Fixed-priority schedulability of a task table (fcn.task_table/1): "
                    "utilisation bounds and exact response-time analysis. Exit status 0: "
                    "schedulable, 1: not schedulable, 2: input refused.")
    parser.add_argument("table", type=Path, help="the task table, JSON")
    parser.add_argument("--json", type=Path, metavar="OUT",
                        help="also write the report as JSON (schema fcn.sched_report/1)")
    args = parser.parse_args(argv)  # a usage error exits with 2 as well
    try:
        table = load_table(args.table)
    except TaskTableError as error:
        print(f"sched: {args.table}: {error}", file=sys.stderr)
        return EXIT_BAD_INPUT
    analysis = analyse(table)
    sys.stdout.write(format_report(analysis, str(args.table)))
    if args.json is not None:
        try:
            with open(args.json, "w", encoding="utf-8", newline="\n") as out:
                json.dump(report_json(analysis, str(args.table)), out, indent=2)
                out.write("\n")
        except OSError as error:
            print(f"sched: cannot write {args.json}: {error}", file=sys.stderr)
            return EXIT_BAD_INPUT
    return EXIT_SCHEDULABLE if analysis.schedulable else EXIT_NOT_SCHEDULABLE


if __name__ == "__main__":
    sys.exit(main())
