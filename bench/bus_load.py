#!/usr/bin/env python3
"""Measure the load the node puts on the bus: every frame captured with the kernel's timestamp,
its exact length with stuff bits, per identifier and in all, against what the DBC's cycle times
promise, on an idle bus and during a download, on both transports.

The frames and their lengths are protocol data, independent of the host; how often the node
sends them per second is the host's timing (docs/measurements.md).

HOW. Each run starts an observer of the bus (python -m fcn_tools.capture: every frame, with the
kernel's receive timestamp, put on CLOCK_MONOTONIC), then the release node at 500 kbit/s:

  idle      the node alone with the observer, --seconds of tick time; the figures cover the run
            from 1 s after the node's first heartbeat to its last
  download  the node started through fcn_boot from a flash with the factory image, and the
            flasher (python -m fcn_tools.flasher --no-reset) downloading the release node into
            the other slot; the figures cover the download, from the tester's first request
            frame (0x7E0) to the node's last response frame (0x7E8)

Per identifier (fcn_tools.busload): frames per second, the exact length of every frame
(fcn_tools.canbits, the same rules as lib/can/can_bits), bits per second with the 3-bit
intermission after each frame, and its share of the 500 kbit/s bus; the total; the node's frames,
the tester's (0x7E0) and the node's diagnostic responses (0x7E8) apart; and the DBC's nominal load
of every cyclic message from its cycle time with its worst-case and its typical stuffed length.
Beside them the node's own counters: its controller model's busy time over its run (lib/can,
busy_ns, frames without intermissions), the frames it sent and received.

    python bench/bus_load.py                         # the final measurement, about 3 minutes
    python bench/bus_load.py --transport socketcan --seconds 120
    python bench/bus_load.py --quick --out /tmp/bl
    python bench/bus_load.py --replot
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from fcn_tools import bench, busload, capture, canbits, paths, plotstyle, results, runner, tester
from fcn_tools import bus as bus_module
from fcn_tools.decode import load_database

NAME = "bus_load"
VERSION = 1
BITRATE = 500_000

#: The defaults of the final measurement: at least 60 s of an idle bus, one download.
SECONDS = 60
DOWNLOADS = 1

#: How long a download run may take at most, and how long the node runs on after it.
DOWNLOAD_RUN_S = 120
AFTER_DOWNLOAD_S = 0.5

HEARTBEAT = 0x030
UDS_REQUEST, UDS_RESPONSE = 0x7E0, 0x7E8
SETTLE_S = 1.0


@dataclass
class Run:
    condition: str
    transport: str
    name: str
    stats: dict
    frames: np.ndarray
    sidecar: dict
    extra: dict


def wait_for_heartbeat(spec: str, node) -> None:
    reader = bus_module.open_bus(spec)
    try:
        first = bus_module.wait_for(reader, bus_module.is_status, timeout=tester.START_WAIT_S)
    finally:
        reader.shutdown()
    if first is None:
        raise runner.BenchFailure(f"{node.name}: no heartbeat within {tester.START_WAIT_S} s\n"
                                  f"{node.stderr()}")


def run_idle(session: runner.Session, workdir: Path, seconds: int, args) -> Run:
    with capture.Observer(session.can_spec, workdir / "idle.capture") as observer:
        node = session.start_node("--duration-s", str(seconds), "--bitrate", str(BITRATE),
                                  "--sensor-seed", str(args.sensor_seed), name="idle")
        status = node.wait(timeout=seconds * 3 + 60)
        if status != 0:
            raise runner.BenchFailure(f"idle ended with status {status}\n{node.stderr()}")
        stats = session.finish(node)
        frames, sidecar = observer.stop()
    return Run("idle", session.transport, "idle", stats, frames, sidecar, {})


def run_download(session: runner.Session, workdir: Path, name: str, images: dict, image: str,
                 args) -> Run:
    flash = workdir / f"{name}.flash"
    bench.factory(flash, images["node-1.0.0"])
    report_path = workdir / f"{name}.flasher.json"
    with capture.Observer(session.can_spec, workdir / f"{name}.capture") as observer:
        node = session.start_node("--bitrate", str(BITRATE), "--sensor-seed",
                                  str(args.sensor_seed), name=name, flash_file=flash,
                                  retained_file=workdir / f"{name}.cell")
        wait_for_heartbeat(session.can_spec, node)
        status, report, stderr = tester.run_flasher(session.can_spec, images[image], report_path,
                                                    "--no-reset")
        if status != 0 or report.get("result") != "committed":
            raise runner.BenchFailure(f"{name}: the flasher ended with status {status}, result "
                                      f"{report.get('result')!r}\n{stderr}")
        time.sleep(AFTER_DOWNLOAD_S)
        stats = session.finish(node)
        frames, sidecar = observer.stop()
    steps = tester.steps_of(report)
    extra = {"image": image, "image_bytes": report["image"]["size"], "result": report["result"],
             "erase_s": steps["erase"]["duration_s"], "transfer_s": steps["transfer"]["duration_s"],
             "blocks": len(report["blocks_s"])}
    return Run("download", session.transport, name, stats, frames, sidecar, extra)


# ----------------------------------------------------------------------------- the analysis


def node_ids(db) -> set[int]:
    """The identifiers the node sends: every message of the DBC with the sender BMS."""
    return {int(message.frame_id) for message in db.messages if "BMS" in message.senders}


def analyse(run: Run, db, expected: list[busload.Nominal]) -> dict:
    for what in ("kernel_drops", "malformed"):
        if run.sidecar[what] != 0:
            raise runner.BenchFailure(f"{run.name}: the observer counted {run.sidecar[what]} "
                                      f"{what.replace('_', ' ')}: it did not see every frame")
    ids = capture.identifiers(run.frames)
    times, trusted = capture.to_monotonic(run.frames)
    try:
        if run.condition == "download":
            # The edges from frames whose times are trusted: one read across a step of the wall
            # clock can be a second off, and would lengthen or shorten the window by that much.
            window = capture.exchange_window(ids, times, UDS_REQUEST, UDS_RESPONSE, trusted)
        else:
            window = capture.steady_window(ids, times, HEARTBEAT, int(SETTLE_S * 1e9), trusted)
    except ValueError as error:
        raise runner.BenchFailure(f"{run.name}: {error}") from None
    lengths = busload.frame_lengths(run.frames["can_id"], run.frames["dlc"], run.frames["data"])
    # A frame belongs to the window by its time; one whose time is not trusted (a step of the
    # wall clock between its arrival and its read) is placed by the time of its read instead,
    # at most the read's delay later.
    placed = np.where(trusted, times, run.frames["read_ns"].astype(np.int64))
    inside = (placed >= window[0]) & (placed <= window[1])
    window_s = (window[1] - window[0]) / 1e9
    by_id = busload.load_by_id(ids[inside], lengths[inside], window_s, BITRATE)
    total, total_bare = busload.total_load(by_id.values())
    mine = node_ids(db)
    groups = {"node": [entry for key, entry in by_id.items() if key in mine],
              "tester": [entry for key, entry in by_id.items() if key == UDS_REQUEST],
              "other": [entry for key, entry in by_id.items()
                        if key not in mine and key != UDS_REQUEST]}
    controller = run.stats["can"]["controller"]
    elapsed = run.stats["can"]["elapsed_ns"]
    # The node's own view over its whole run: what its model counted as busy, against the
    # captured frames over the same span (frames between the node's first and last frame).
    node_frame = np.isin(ids, list(mine))
    span = (placed >= placed[node_frame].min()) & (placed <= placed[node_frame].max())
    span_s = (placed[node_frame].max() - placed[node_frame].min()) / 1e9
    captured_bits = int(lengths[span].sum())
    return {
        "name": run.name, "window_s": round(window_s, 6),
        "frames_in_window": int(inside.sum()), "observer_frames": int(run.frames.size),
        "wall_clock_steps": run.sidecar["clock_steps"],
        "times_not_trusted": int((~trusted).sum()),
        "by_id": {f"{key:#05x}": entry.as_json() for key, entry in sorted(by_id.items())},
        "load": round(total, 6), "load_without_intermission": round(total_bare, 6),
        "groups": {name: {"frames_per_s": round(sum(e.frames_per_s for e in entries), 3),
                          "bits_per_s": round(sum(e.bits_per_s for e in entries), 3),
                          "load": round(sum(e.load for e in entries), 6)}
                   for name, entries in groups.items()},
        "nominal": busload.compare(by_id, expected, BITRATE),
        "node_counters": {
            "elapsed_s": round(elapsed / 1e9, 6), "busy_s": round(controller["busy_ns"] / 1e9, 6),
            "model_busy_share": round(controller["busy_ns"] / elapsed, 6) if elapsed else None,
            "tx_frames": controller["tx_frames"], "rx_frames": controller["rx_frames"],
            "wire_overruns": controller["wire_overruns"],
            # Ticks the node gave up (posix_tick.c): its periods, and so its frames per second
            # of the wall clock, fall short by them.
            "tick_dropped": run.stats["tick"]["dropped"],
            "transport_rx_frames": run.stats["can"]["transport_counters"]["rx_frames"],
            "captured_node_frames": int(node_frame.sum()),
            "captured_other_frames": int((~node_frame).sum()),
            "captured_share_without_intermission": (round(captured_bits / BITRATE / span_s, 6)
                                                    if span_s > 0 else None)},
        "run": run.extra,
    }


def check_counts(run: Run, entry: dict) -> None:
    """The observer saw every frame the node's model completed, and nothing the node sent that
    its model did not: counts independent of the host's timing."""
    counters = entry["node_counters"]
    if counters["captured_node_frames"] != counters["tx_frames"]:
        raise runner.BenchFailure(f"{run.name}: the observer saw {counters['captured_node_frames']}"
                                  f" frames of the node, its controller sent "
                                  f"{counters['tx_frames']}")


# ------------------------------------------------------------------------------ the result


def summary_of(data: dict) -> dict:
    out: dict = {}
    for transport, conditions in data["transports"].items():
        idle = conditions["idle"]
        group: dict = {
            "idle_load": results.figure(idle["load"], "fraction of 500 kbit/s"),
            "idle_load_without_intermission":
                results.figure(idle["load_without_intermission"], "fraction of 500 kbit/s"),
            "idle_nominal_load_typical": results.figure(data["nominal"]["load_typical"],
                                                        "fraction of 500 kbit/s"),
            "idle_nominal_load_worst_case": results.figure(data["nominal"]["load_worst_case"],
                                                           "fraction of 500 kbit/s"),
            "idle_model_busy_share": results.figure(idle["node_counters"]["model_busy_share"],
                                                    "fraction of the run"),
        }
        download = conditions["download"]
        group["download_load"] = results.figure(download["load"], "fraction of 500 kbit/s")
        group["download_tester_load"] = results.figure(download["groups"]["tester"]["load"],
                                                       "fraction of 500 kbit/s")
        group["download_node_diag_load"] = results.figure(
            download["by_id"].get(f"{UDS_RESPONSE:#05x}", {}).get("load", 0.0),
            "fraction of 500 kbit/s")
        group["download_node_load"] = results.figure(download["groups"]["node"]["load"],
                                                     "fraction of 500 kbit/s")
        out[transport] = group
    return out



def measure(binaries: runner.Binaries, args) -> tuple[dict, dict[str, str]]:
    db = load_database()
    expected = busload.nominal(db.messages, senders=["BMS"])
    workroot = runner.bench_workdir(NAME)
    (workroot / "images").mkdir()
    images = bench.make_images(workroot / "images", paths.build_root() / "release" / "bin")
    image = "data" if args.quick else "node-1.1.0"
    data: dict = {"nominal": {
        "messages": {entry.name: {"id": entry.can_id, "cycle_ms": entry.cycle_ms,
                                  "dlc": entry.dlc, "worst_case_bits": entry.worst_bits,
                                  "typical_bits": round(entry.typical_bits, 3),
                                  "load_worst_case": round(entry.load(BITRATE, entry.worst_bits),
                                                           6),
                                  "load_typical": round(entry.load(BITRATE, entry.typical_bits), 6)}
                     for entry in expected},
        "load_worst_case": round(sum(entry.load(BITRATE, entry.worst_bits)
                                     for entry in expected), 6),
        "load_typical": round(sum(entry.load(BITRATE, entry.typical_bits)
                                  for entry in expected), 6),
        "typical_draws": busload.NOMINAL_DRAWS, "typical_seed": busload.NOMINAL_SEED},
        "transports": {}}
    reported: dict[str, str] = {}
    for transport in args.transports:
        workdir = workroot / transport
        print(f"{transport}: idle, {args.seconds} s ...", flush=True)
        with runner.Session(workdir, transport, binaries=binaries) as session:
            idle = run_idle(session, workdir, args.seconds, args)
            downloads = []
            for index in range(args.downloads):
                print(f"{transport}: download {index + 1} of {args.downloads} ({image}) ...",
                      flush=True)
                downloads.append(run_download(session, workdir, f"download-{index + 1}", images,
                                              image, args))
        conditions = {}
        entry = analyse(idle, db, expected)
        check_counts(idle, entry)
        conditions["idle"] = entry
        reported[f"{transport} idle"] = idle.stats["build_id"]
        entries = []
        for run in downloads:
            found = analyse(run, db, expected)
            check_counts(run, found)
            entries.append(found)
            reported[f"{transport} {run.name}"] = run.stats["build_id"]
        # Every download's window, pooled (busload.pool()); each run's own figures in "runs".
        conditions["download"] = busload.pool(entries, BITRATE)
        data["transports"][transport] = conditions
    data["workdir"] = str(workroot)
    return data, reported


def config_of(args, binaries: runner.Binaries) -> dict:
    return {"bitrate": BITRATE, "transports": list(args.transports), "seconds": args.seconds,
            "downloads": args.downloads, "download_image": "data" if args.quick else "node-1.1.0",
            "settle_s": SETTLE_S, "after_download_s": AFTER_DOWNLOAD_S,
            "intermission_bits": canbits.INTERMISSION_BITS, "sensor_seed": args.sensor_seed,
            "quick": args.quick, "binaries": binaries.origin}


TIMING_NOTE = (
    "The frames, their lengths and the nominal load are protocol data and do not depend on the "
    "host; the frames per second do, through when the node's tasks ran, and the node's frames "
    "are paced by its controller model at 500 kbit/s while the tester's frames reach the "
    "observer when the tester sends them, not when a real bus would carry them.")


# ------------------------------------------------------------------------------------ the plot


def plot(result: dict, path: Path) -> None:
    import matplotlib.pyplot as plt

    plotstyle.apply()
    transports = list(result["transports"])
    fig, axes = plt.subplots(1, 2, figsize=(11.0, 4.8))
    fig.subplots_adjust(left=0.07, right=0.98, top=0.74, bottom=0.2, wspace=0.25)

    ax = axes[0]
    names = list(result["nominal"]["messages"])
    positions = np.arange(len(names))
    width = 0.8 / (2 + len(transports))
    nominal = result["nominal"]["messages"]
    ax.bar(positions - width, [nominal[name]["load_worst_case"] * 100 for name in names], width,
           color=plotstyle.SERIES[3], label="DBC nominal, worst-case stuffing")
    ax.bar(positions, [nominal[name]["load_typical"] * 100 for name in names], width,
           color=plotstyle.SERIES[4], label="DBC nominal, typical (random data)")
    for index, transport in enumerate(transports):
        measured = result["transports"][transport]["idle"]["nominal"]
        ax.bar(positions + width * (index + 1),
               [measured[name]["measured_load"] * 100 for name in names], width,
               color=plotstyle.SERIES[index], label=f"measured on {transport}")
    ax.set_xticks(positions, [name.replace("BMS_", "") for name in names], rotation=35,
                  ha="right")
    ax.set_ylabel("share of the 500 kbit/s bus (%)")
    ax.set_title("The node's cyclic frames, idle bus")
    plotstyle.legend_above(ax)

    ax = axes[1]
    labels = []
    bars: dict[str, list[float]] = {"node": [], "tester": [], "other": []}
    for transport in transports:
        for condition in ("idle", "download"):
            groups = result["transports"][transport][condition]["groups"]
            labels.append(f"{transport}\n{condition}")
            for name in bars:
                bars[name].append(groups[name]["load"] * 100)
    positions = np.arange(len(labels))
    bottom = np.zeros(len(labels))
    colours = {"node": plotstyle.SERIES[0], "tester": plotstyle.SERIES[1],
               "other": plotstyle.SERIES[2]}
    for name, values in bars.items():
        ax.bar(positions, values, 0.6, bottom=bottom, color=colours[name],
               label={"node": "the node's frames", "tester": "the tester's requests (0x7E0)",
                      "other": "other frames"}[name])
        bottom += np.asarray(values)
    ax.set_xticks(positions, labels)
    ax.set_ylabel("share of the 500 kbit/s bus (%)")
    ax.set_title("Bus load with the intermission, by sender")
    plotstyle.legend_above(ax)

    host = "WSL2" if result["provenance"]["host"]["is_wsl"] else "Linux"
    plotstyle.footnote(fig, f"bench/bus_load.py, {result['config']['seconds']} s idle, "
                            f"{result['config']['downloads']} download(s); exact stuffed frame "
                            f"lengths; FreeRTOS POSIX port on a {host} host.")
    plotstyle.save(fig, path)


# -------------------------------------------------------------------------------------- main


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    results.add_bench_arguments(parser, quick="4 s idle, one download of the small data image")
    parser.add_argument("--transport", choices=("both", *runner.TRANSPORTS), default="both",
                        help="the transports to measure on (default both)")
    parser.add_argument("--seconds", type=int, default=None,
                        help=f"tick time of the idle run (default {SECONDS}, at least 60 for a "
                             "full run)")
    parser.add_argument("--downloads", type=int, default=None,
                        help=f"runs with a download (default {DOWNLOADS})")
    parser.add_argument("--sensor-seed", type=int, default=1,
                        help="the sensor model's seed (default %(default)s)")
    args = parser.parse_args(argv)
    args.seconds = args.seconds or (4 if args.quick else SECONDS)
    args.downloads = args.downloads if args.downloads is not None else DOWNLOADS
    args.transports = runner.TRANSPORTS[::-1] if args.transport == "both" else (args.transport,)
    if args.downloads < 1 or args.seconds < 3:
        parser.error("--downloads must be at least 1 and --seconds at least 3")
    if not args.quick and args.seconds < 60:
        parser.error("a full run captures at least 60 s (--seconds); --quick is the test")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    out = results.output_dir(args)
    if args.replot:
        path = out / f"{NAME}.json"
        if not path.exists():
            print(f"nothing to replot: {path} does not exist", file=sys.stderr)
            return 1
        plot(json.loads(path.read_text()), path.with_suffix(".png"))
        print(f"redrew {path.with_suffix('.png')}")
        return 0
    try:
        load = results.check_load(args.max_load, args.allow_loaded)
        binaries = runner.prepare_binaries(build=not args.no_build)
        results.require_tree_build(binaries.as_dict())
        data, reported = measure(binaries, args)
        path = results.write_result(NAME, data, summary_of(data), config_of(args, binaries), out,
                                    version=VERSION, timing_note=TIMING_NOTE, load=load,
                                    binaries=binaries.as_dict(), reported_build_ids=reported,
                                    binary_sha256=binaries.sha256)
    except results.HostLoaded as error:
        print(error, file=sys.stderr)
        return results.EXIT_HOST_LOADED
    except results.BuildIdMismatch as error:
        print(f"no result written: {error}", file=sys.stderr)
        return results.EXIT_BUILD_MISMATCH
    except KeyboardInterrupt as error:
        return runner.interrupted_status(error)
    except (AssertionError, RuntimeError, ValueError) as error:
        # A node or an observation that is not what the analysis needs, or a bus that is not
        # there (vcan0, scripts/vcan-up.sh): no result.
        print(f"no result written: {error}", file=sys.stderr)
        return 1
    plot(json.loads(path.read_text()), path.with_suffix(".png"))
    for transport, conditions in data["transports"].items():
        for condition in ("idle", "download"):
            entry = conditions[condition]
            print(f"{transport:9s} {condition:8s} load {entry['load'] * 100:.3f} % of 500 kbit/s "
                  f"(without intermissions {entry['load_without_intermission'] * 100:.3f} %), "
                  f"node {entry['groups']['node']['load'] * 100:.3f} %, tester "
                  f"{entry['groups']['tester']['load'] * 100:.3f} %")
    print(f"nominal: typical {data['nominal']['load_typical'] * 100:.3f} %, worst case "
          f"{data['nominal']['load_worst_case'] * 100:.3f} %")
    print(f"wrote {path} and {path.with_suffix('.png')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
