#!/usr/bin/env python3
"""Run every bench for the final results: one at a time, each on an idle machine, stopping at
the first failure, and bench/platform_timing.py last.

    python bench/run_all.py                    # the whole plan (see --list), hours
    python bench/run_all.py --list             # the plan, without running anything
    python bench/run_all.py --from NAME        # resume at NAME, after a failure
    python bench/run_all.py --only NAME        # one bench of the plan

What it does, in order:
  0. Refuses to do anything (exit status 4, results.EXIT_BUILD_MISMATCH) unless this script is
     the file of the checkout fcn_tools is imported from (results.script_mismatch()): it runs
     that checkout's benches, and a run_all.py of another checkout, with another plan, would
     run them in its name. Every bench it starts lies in that checkout and checks itself the
     same way.
  1. Refuses a tree with uncommitted changes (--allow-dirty overrides): a final result must
     name a commit in its provenance, not a dirty tree.
  2. Builds the release preset once (scripts/build.sh release); every bench builds it again,
     which then does nothing, and checks that its binaries carry the tree's build id.
  3. For each bench of the plan: waits until the 1-minute load average is at most --max-load
     (the limit the benches refuse above, fcn_tools.results.DEFAULT_LOAD_LIMIT), for at most
     --idle-timeout seconds (the previous bench's own load takes a minute or two to decay), then
     runs it with this interpreter from the repository root, with its default arguments: the
     final measurement, written to results/. A bench that exits with any status but 0 ends the
     run with that status; the benches after it are not run.

Each bench writes results/<name>.json and results/<name>.png (bench/README.md). The lead commits
them after the run; nothing here touches git.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Sequence

from fcn_tools import paths, results


@dataclass(frozen=True)
class Bench:
    """One step of the plan: its name (that of its result), and the command after the
    interpreter, relative to the repository root."""
    name: str
    argv: tuple[str, ...]


#: The plan, in the order of the final runs (bench/README.md says why). Each bench of phase 6
#: adds its line above platform_timing, which stays last.
PLAN: tuple[Bench, ...] = (
    Bench("node_timing", ("bench/node_timing.py",)),
    Bench("bus_load", ("bench/bus_load.py",)),
    Bench("bus_off", ("bench/bus_off.py",)),
    Bench("isotp_throughput", ("bench/isotp_throughput.py",)),
    Bench("updates", ("bench/updates.py",)),
    Bench("power_loss", ("bench/power_loss.py",)),
    Bench("platform_timing", ("bench/platform_timing.py",)),
)

#: How often the load is looked at while waiting for an idle machine, and for how long at most.
IDLE_POLL_S = 10.0
IDLE_TIMEOUT_S = 900.0


def wait_for_idle(limit: float, timeout: float, *, poll: float = IDLE_POLL_S,
                  load: Callable[[], float] = lambda: os.getloadavg()[0],
                  sleep: Callable[[float], None] = time.sleep,
                  clock: Callable[[], float] = time.monotonic,
                  say: Callable[[str], None] = print) -> float:
    """Wait until the 1-minute load average is at most `limit`; the seconds waited.
    TimeoutError if it is still above it after `timeout` seconds."""
    start = clock()
    first = True
    while True:
        now = load()
        if now <= limit:
            return clock() - start
        if clock() - start >= timeout:
            raise TimeoutError(f"the 1-minute load average is still {now:.2f}, above {limit}, "
                               f"after {timeout:.0f} s: something else runs on this machine")
        if first:
            say(f"waiting for an idle machine: load average {now:.2f}, limit {limit}")
            first = False
        sleep(poll)


def select(plan: Sequence[Bench], start: str | None = None,
           only: str | None = None) -> list[Bench]:
    """The benches of `plan` to run: all, from `start` on, or `only` that one. ValueError for a
    name that is not in the plan."""
    names = [bench.name for bench in plan]
    for name in (start, only):
        if name is not None and name not in names:
            raise ValueError(f"no bench {name} in the plan ({', '.join(names)})")
    if only is not None:
        return [plan[names.index(only)]]
    if start is not None:
        return list(plan[names.index(start):])
    return list(plan)


def run_plan(benches: Sequence[Bench], *, wait: Callable[[], float],
             run: Callable[[Bench], int], say: Callable[[str], None] = print) -> int:
    """Run `benches` in order, each after wait(); the exit status of the first that fails
    (0 if none does). A failing bench stops the plan."""
    done: list[tuple[str, int, float]] = []
    status = 0
    for bench in benches:
        waited = wait()
        say(f"=== {bench.name}: {' '.join(bench.argv)} (waited {waited:.0f} s for an idle "
            "machine)")
        start = time.monotonic()
        status = run(bench)
        done.append((bench.name, status, time.monotonic() - start))
        if status != 0:
            break
    for name, code, seconds in done:
        say(f"{name:24s} exit {code:3d}  {seconds / 60:7.1f} min")
    left = [bench.name for bench in benches[len(done):]]
    if status != 0:
        say(f"stopped at {done[-1][0]} (exit {status}); not run: {', '.join(left) or 'none'}")
    return status


def dirty_files() -> list[str]:
    """The tracked files that differ from the commit (results/ aside), as a result's provenance
    lists them."""
    return results.provenance()["git_modified_files"]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--list", action="store_true", help="print the plan and exit")
    parser.add_argument("--from", dest="start", metavar="NAME", help="start at this bench")
    parser.add_argument("--only", metavar="NAME", help="run only this bench")
    parser.add_argument("--max-load", type=float, default=results.DEFAULT_LOAD_LIMIT,
                        metavar="L", help="wait for a 1-minute load average of at most L "
                                          f"before each bench (default {results.DEFAULT_LOAD_LIMIT})")
    parser.add_argument("--idle-timeout", type=float, default=IDLE_TIMEOUT_S, metavar="S",
                        help=f"give up waiting for it after S seconds (default {IDLE_TIMEOUT_S:.0f})")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="run on a tree with uncommitted changes (the results then name a "
                             "dirty tree: not for the final runs)")
    args = parser.parse_args(argv)

    # This file, not sys.argv[0]: the tests load and run main() in pytest.
    foreign = results.script_mismatch(Path(__file__))
    if foreign is not None:
        print(f"no bench run: {foreign}", file=sys.stderr)
        return results.EXIT_BUILD_MISMATCH
    try:
        benches = select(PLAN, args.start, args.only)
    except ValueError as error:
        parser.error(str(error))
    if args.list:
        for bench in benches:
            print(f"{bench.name:24s} python {' '.join(bench.argv)}")
        return 0

    dirty = dirty_files()
    if dirty and not args.allow_dirty:
        print("the tree has uncommitted changes, so no result would name a commit: "
              f"{', '.join(dirty)} (commit them, or pass --allow-dirty)", file=sys.stderr)
        return 1
    print("building the release preset (scripts/build.sh release) ...", flush=True)
    subprocess.run(["bash", str(paths.REPO_ROOT / "scripts" / "build.sh"), "release"],
                   check=True)

    def wait() -> float:
        return wait_for_idle(args.max_load, args.idle_timeout)

    def run(bench: Bench) -> int:
        return subprocess.run([sys.executable, *bench.argv], cwd=paths.REPO_ROOT,
                              check=False).returncode

    try:
        return run_plan(benches, wait=wait, run=run)
    except TimeoutError as error:
        print(f"run_all: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
