"""Slices of real traces of the node (traces/*.txt: the text at the top of each file says where it
comes from and what it holds), read into fcn_tools.trace.Trace for the tests of the analysis."""
from __future__ import annotations

from pathlib import Path

import numpy as np

from fcn_tools.trace import EVENT_DTYPE, Trace

TRACES = Path(__file__).parent / "traces"

#: The columns of a slice's lines, fields of fcn_tools.trace.EVENT_DTYPE.
COLUMNS = ("kind", "time_ns", "value", "arg", "task", "other", "flags")


def load(name: str) -> Trace:
    """The slice traces/`name`, with the task names of its "# tasks:" line."""
    names: dict[int, str] = {}
    rows: list[list[int]] = []
    for line in (TRACES / name).read_text().splitlines():
        if line.startswith("# tasks:"):
            words = line.split()[2:]
            names = {int(number): word for number, word in zip(words[::2], words[1::2])}
        elif line and not line.startswith("#"):
            rows.append([int(field) for field in line.split()])
    events = np.zeros(len(rows), dtype=EVENT_DTYPE)
    table = np.asarray(rows, dtype=np.uint64)
    for column, field in enumerate(COLUMNS):
        events[field] = table[:, column]
    return Trace(events=events, names=names)
