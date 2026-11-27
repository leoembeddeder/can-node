#!/usr/bin/env python3
"""Measure the two host mechanisms that all of the node's timing rests on.

Everything measured here is the timing of a Linux host running the FreeRTOS POSIX port, not of
a microcontroller: the platform layer removes the port's systematic errors, the host's
scheduler still decides when a thread runs.

1. Tick accuracy. The FreeRTOS POSIX port generates its tick with a relative sleep and so
   runs slow against the wall clock; platform/posix/posix_tick.c locks the tick count to
   CLOCK_MONOTONIC. Both are measured with a 10 ms periodic task.
2. Simulated interrupt latency. Time from a peripheral model raising an interrupt line to the
   task it wakes, for every raise, with signal delivery and with tick-hook polling
   (platform/posix/posix_irq.c), while a mid-priority task loads the scheduler.
3. Interrupt storm. The same line raised in a tight loop for a few seconds: every raise is
   served and interrupt handlers never stack up on a task's stack.

Builds the release preset first (scripts/build.sh release), so the binary measured is the one
the tree describes, then runs its tests/platform/plat_selftest and writes
results/platform_timing.json and results/platform_timing.png. The self-test reports the build
id it was compiled with (computed at build time: the commit and, for a tree with uncommitted
changes, a hash of those changes); a result is only written if that id equals the id of the
tree its provenance records, as the tree is when the result is written: the commit and the
content of the uncommitted changes, not only whether there are any. FCN_SELFTEST names
another binary to measure; nothing is built then, and the same check applies, so a binary
built from a different set of uncommitted changes is refused as well.

As every bench of phase 6 does (fcn_tools.results' load check), it refuses to measure when the
1-minute load average at its start is above --max-load (default 1.0), exit status 3, unless
--allow-loaded makes it measure and mark the result "loaded": true; --out DIR writes the result
and the figure into DIR instead of results/. Every other refusal exits 1, as before phase 6.

    python bench/platform_timing.py                 # 10 s per run, about a minute in total
    python bench/platform_timing.py --seconds 60    # longer runs, about 4.5 minutes
    python bench/platform_timing.py --seconds 1 --out /tmp/pt   # a test of the bench
    python bench/platform_timing.py --replot        # redraw the figure from the last run
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

from fcn_tools import paths, plotstyle, results
# The build id of the tree a result names (the same rule as cmake/FcnBuildId.cmake) and the
# check of the self-test's ids against it live in fcn_tools.results, shared with every bench.
from fcn_tools.results import build_id_mismatch, tree_build_id  # noqa: F401

IRQ_PERIOD_US = 250  # about the frame rate of a saturated 500 kbit/s CAN bus
STORM_SECONDS_MAX = 5
TIMING_MISSED = 75  # plat_selftest: the host did not keep time well enough for the test


def run_selftest(binary: Path, args: list[str], timeout: float) -> tuple[dict, np.ndarray]:
    """Run one self-test; return its JSON report and its raw samples in nanoseconds."""
    with tempfile.TemporaryDirectory() as tmp:
        dump = Path(tmp) / "samples.txt"
        env = dict(os.environ, FCN_SELFTEST_DUMP=str(dump))
        proc = subprocess.run([str(binary), *args], capture_output=True, text=True,
                              timeout=timeout, env=env)
        if proc.returncode != 0:
            why = (" (a timing tolerance was missed: the host did not run the test well "
                   "enough to measure it)" if proc.returncode == TIMING_MISSED else "")
            raise RuntimeError(f"plat_selftest {' '.join(args)} exited {proc.returncode}{why}: "
                               f"{proc.stderr.strip()}")
        report = json.loads(proc.stdout.strip().splitlines()[-1])
        samples = np.loadtxt(dump, dtype=np.float64, ndmin=1)
    return report, samples


def measure(binary: Path, seconds: int) -> tuple[dict, dict, set[str]]:
    """Run every measurement; return the data, the raw samples and the build ids reported."""
    data: dict = {"seconds_per_run": seconds, "tick": {}, "interrupt": {}, "interrupt_storm": {}}
    raw: dict = {}
    build_ids: set[str] = set()
    timeout = seconds * 3 + 30

    for mode in ("stock", "locked"):
        print(f"tick, {mode} mode, {seconds} s ...", flush=True)
        report, samples = run_selftest(binary, ["tick", mode, str(seconds)], timeout)
        build_ids.add(report["build_id"])
        raw[f"tick_{mode}"] = samples
        data["tick"][mode] = {
            "nominal_tick_period_us": report["nominal_tick_period_us"],
            "effective_tick_period_us": report["effective_tick_period_us"],
            "error_percent": round(
                (report["effective_tick_period_us"] / report["nominal_tick_period_us"] - 1) * 100,
                3),
            "drift_over_run_us": report["drift_us"],
            "tick_signals_sent": report["signals_sent"],
            "catch_up_ticks": report["catch_ups"],
            # Raises skipped because a made-up tick had been one too many (posix_tick_plan.h).
            "given_back_ticks": report["give_backs"],
            "dropped_ticks": report["dropped_ticks"],
            "dropped_ticks_in_measured_span": report["dropped_ticks_in_span"],
            # Stalls of the whole process, seen by a witness thread outside the scheduler, and
            # the dropped ticks they do not explain (see plat_selftest.c, "dropped ticks").
            "host_stalls_seen": report["host_stalls_seen"],
            "host_stall_us": report["host_stall_us"],
            "dropped_not_explained_by_host_stalls": report["dropped_unexcused"],
            "unaccounted_time_percent": report["unaccounted_time_percent"],
            "wake_interval_of_10ms_task_us": results.percentiles(samples, 1e3),
        }

    for mode in ("signal", "poll"):
        print(f"interrupt latency, {mode} mode, {seconds} s ...", flush=True)
        report, samples = run_selftest(
            binary, ["irq", mode, str(IRQ_PERIOD_US), str(seconds), "1"], timeout)
        build_ids.add(report["build_id"])
        raw[f"irq_{mode}"] = samples
        data["interrupt"][mode] = {
            "nominal_raise_period_us": report["nominal_period_us"],
            "mean_raise_period_us": report["mean_raise_period_us"],
            "raised": report["raised"],
            "task_wakeups": report["task_wakeups"],
            "unserved_at_end": report["unserved_at_end"],
            "serviced_by_signal_handler": report["serviced_by_signal"],
            "serviced_by_tick_hook": report["serviced_by_tick"],
            "max_handler_nesting": report["max_handler_nesting"],
            # One sample per raise: from the raise to the task's first run after it. n is the
            # number of raises, less those the self-test could not sample (counted here).
            "raises_without_latency_sample": report["latency_samples_not_recorded"],
            "raise_to_task_latency_per_interrupt_us": results.percentiles(samples, 1e3),
        }

    # An interrupt storm: the source raises in a tight loop. What matters here is not a
    # latency but that the node survives it: every raise served, handlers never stacked.
    storm_seconds = min(seconds, STORM_SECONDS_MAX)
    for mode in ("signal", "poll"):
        print(f"interrupt storm, {mode} mode, {storm_seconds} s ...", flush=True)
        report, _ = run_selftest(binary, ["irq", mode, "0", str(storm_seconds), "1"], timeout)
        build_ids.add(report["build_id"])
        data["interrupt_storm"][mode] = {
            "seconds": storm_seconds,
            "raised": report["raised"],
            "raises_per_second": round(report["raised"] / storm_seconds),
            "task_wakeups": report["task_wakeups"],
            "unserved_at_end": report["unserved_at_end"],
            "max_handler_nesting": report["max_handler_nesting"],
            "stack_peak_max_bytes": report["stack_peak_max_bytes"],
        }

    return data, raw, build_ids


def plot(raw: dict, data: dict, path: Path) -> None:
    import matplotlib.pyplot as plt

    plotstyle.apply()
    fig, (ax_tick, ax_irq) = plt.subplots(1, 2, figsize=(10.0, 4.3))
    fig.subplots_adjust(left=0.07, right=0.98, top=0.76, bottom=0.18, wspace=0.22)
    series = {"locked": ("locked tick (this project)", plotstyle.SERIES[0]),
              "stock": ("stock POSIX port", plotstyle.SERIES[1])}

    # Left: distribution of the wake-up interval of a 10 ms task.
    all_ms = np.concatenate([raw["tick_locked"], raw["tick_stock"]]) / 1e6
    low, high = np.percentile(all_ms, [0.2, 99.8])
    bins = np.linspace(min(low, 9.0), max(high, 11.0), 90)
    for mode in ("stock", "locked"):
        label, colour = series[mode]
        tick = data["tick"][mode]
        ax_tick.hist(raw[f"tick_{mode}"] / 1e6, bins=bins, histtype="stepfilled", alpha=0.22,
                     color=colour, linewidth=0, weights=np.full(raw[f"tick_{mode}"].size,
                                                                100.0 / raw[f"tick_{mode}"].size))
        ax_tick.hist(raw[f"tick_{mode}"] / 1e6, bins=bins, histtype="step", color=colour,
                     linewidth=1.6, label=f"{label}: tick = {tick['effective_tick_period_us']:.1f} µs",
                     weights=np.full(raw[f"tick_{mode}"].size, 100.0 / raw[f"tick_{mode}"].size))
    plotstyle.reference_line(ax_tick, 10.0, "nominal 10 ms")
    ax_tick.set_title("Wake-up interval of a 10 ms periodic task")
    ax_tick.set_xlabel("interval between consecutive wake-ups (ms)")
    ax_tick.set_ylabel("share of wake-ups (%)")
    plotstyle.legend_above(ax_tick)

    # Right: cumulative distribution of the latency of every interrupt, raise to task.
    irq_series = {"signal": ("signal delivery", plotstyle.SERIES[0]),
                  "poll": ("tick-hook polling", plotstyle.SERIES[1])}
    for mode in ("signal", "poll"):
        label, colour = irq_series[mode]
        us = np.sort(raw[f"irq_{mode}"]) / 1e3
        share = np.arange(1, us.size + 1) / us.size * 100.0
        stats = data["interrupt"][mode]["raise_to_task_latency_per_interrupt_us"]
        ax_irq.plot(us, share, color=colour,
                    label=f"{label}: median {stats['p50']:.0f} µs (n = {stats['n']} raises)")
    ax_irq.set_xscale("log")
    ax_irq.set_xlim(10, 5000)
    ax_irq.set_ylim(0, 100)
    plotstyle.reference_line(ax_irq, 1000.0, "1 tick")
    ax_irq.set_title("Simulated interrupt: every raise, to the task running")
    ax_irq.set_xlabel("latency from raise to task (µs, log scale)")
    ax_irq.set_ylabel("share of interrupts served within (%)")
    plotstyle.legend_above(ax_irq)

    host = "WSL2" if data["provenance"]["host"]["is_wsl"] else "Linux"
    plotstyle.footnote(fig, f"bench/platform_timing.py, {data['seconds_per_run']} s per run, "
                            f"FreeRTOS POSIX port on a {host} host. Host timing, not MCU timing.")
    plotstyle.save(fig, path)


def selftest_binary() -> tuple[Path, str]:
    """Build the release preset unless FCN_SELFTEST names a binary; return it and its origin."""
    if os.environ.get("FCN_SELFTEST"):
        return paths.selftest_binary("release"), "FCN_SELFTEST"
    print("building the release preset (scripts/build.sh release) ...", flush=True)
    subprocess.run(["bash", str(paths.REPO_ROOT / "scripts" / "build.sh"), "release"],
                   check=True)
    return paths.selftest_binary("release"), "scripts/build.sh release"


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--seconds", type=int, default=10, help="duration of each of the 4 runs")
    parser.add_argument("--replot", action="store_true",
                        help="only redraw the plot from the samples of the last run")
    parser.add_argument("--out", type=Path, default=None, metavar="DIR",
                        help="write platform_timing.json and .png into DIR (default: results/)")
    parser.add_argument("--max-load", type=float, default=results.DEFAULT_LOAD_LIMIT,
                        metavar="L", help="refuse to measure when the 1-minute load average at "
                        f"the start is above L (default {results.DEFAULT_LOAD_LIMIT})")
    parser.add_argument("--allow-loaded", action="store_true",
                        help="measure even above --max-load; the result then says "
                             "\"loaded\": true")
    args = parser.parse_args()

    out_dir = paths.results_dir() if args.out is None else args.out
    json_path = out_dir / "platform_timing.json"
    png_path = json_path.with_suffix(".png")
    # The raw samples are kept next to the build, not in the repository; --replot redraws the
    # figure from them without measuring again.
    raw_path = paths.build_root() / "bench" / "platform_timing_raw.npz"

    if args.replot:
        if not (json_path.exists() and raw_path.exists()):
            print("nothing to replot: run without --replot first", file=sys.stderr)
            return 1
        plot(dict(np.load(raw_path)), json.loads(json_path.read_text()), png_path)
        print(f"redrew {png_path}")
        return 0

    try:
        load = results.check_load(args.max_load, args.allow_loaded)
    except ValueError as error:     # a limit that is no positive number
        parser.error(str(error))
    except results.HostLoaded as error:
        print(error, file=sys.stderr)
        return results.EXIT_HOST_LOADED

    binary, origin = selftest_binary()
    if not binary.exists():
        print(f"{binary} not found", file=sys.stderr)
        return 1

    # The binary must not change while it is measured, so its hash is taken before and after.
    binary_sha256 = sha256_of(binary)
    data, raw, build_ids = measure(binary, args.seconds)
    if sha256_of(binary) != binary_sha256:
        print(f"{binary} changed while it was measured; no result written", file=sys.stderr)
        return 1

    # Checked against the tree as it is now, just before the provenance is recorded, and again
    # against the provenance that was actually written.
    mismatch = build_id_mismatch(build_ids, results.provenance())
    if mismatch is not None:
        print(f"no result written: {mismatch}", file=sys.stderr)
        return 1
    data["binary"] = {"path": str(binary), "sha256": binary_sha256,
                      "build_id": next(iter(build_ids)), "origin": origin}

    previous = json_path.read_bytes() if json_path.exists() else None
    results.write_result("platform_timing", data, out_dir=out_dir, load=load)
    mismatch = build_id_mismatch(build_ids, json.loads(json_path.read_text())["provenance"])
    if mismatch is not None:
        # The tree changed in the moment between the check and the write: put back what was
        # there, so that no result names a tree its binary was not built from.
        if previous is None:
            json_path.unlink()
        else:
            json_path.write_bytes(previous)
        print(f"no result written: {mismatch}", file=sys.stderr)
        return 1

    raw_path.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(raw_path, **raw)
    plot(raw, json.loads(json_path.read_text()), png_path)

    for mode in ("stock", "locked"):
        tick = data["tick"][mode]
        print(f"tick {mode:6s}: {tick['effective_tick_period_us']:9.3f} us per tick "
              f"({tick['error_percent']:+.3f} %), dropped {tick['dropped_ticks_in_measured_span']}")
    for mode in ("signal", "poll"):
        irq = data["interrupt"][mode]
        lat = irq["raise_to_task_latency_per_interrupt_us"]
        print(f"irq  {mode:6s}: one per {irq['mean_raise_period_us']:.1f} us, n {lat['n']} of "
              f"{irq['raised']} raises, p50 {lat['p50']} us, p99 {lat['p99']} us, "
              f"max {lat['max']} us, unserved {irq['unserved_at_end']}")
    for mode in ("signal", "poll"):
        storm = data["interrupt_storm"][mode]
        print(f"storm {mode:6s}: {storm['raises_per_second']} raises/s, "
              f"{storm['task_wakeups']} wake-ups, handler nesting {storm['max_handler_nesting']}, "
              f"stack peak {storm['stack_peak_max_bytes']} bytes, "
              f"unserved {storm['unserved_at_end']}")
    print(f"wrote {json_path} and {png_path} (binary {data['binary']['build_id']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
