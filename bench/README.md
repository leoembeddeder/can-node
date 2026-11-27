# Benches

Every measurement the README quotes comes from a bench here: a script `bench/<name>.py` that
writes `results/<name>.json` (the data, with its provenance) and `results/<name>.png` (its plot).
Every measured figure is one of the FreeRTOS POSIX port on a Linux host, not of a
microcontroller, and a result says which of its figures are computed instead (a standard's
constant, the DBC's nominal load, the bus time of known frames, a projection) and which do not
depend on the host: each result carries `HOST_TIMING_NOTE` (tools/fcn_tools/results.py) with
the bench's own sentence, bus_off gives each of its times and bit counts a `part` (`standard`,
`model`, `policy`, `host` or `mixed`), and docs/measurements.md says for each bench what it
measures, exactly how, and what the figure does and does not say about a target.

## Running a bench

From the repository root, with the project's virtual environment (scripts/setup-python.sh):

```sh
export PYTHONPATH=$PWD/tools        # fcn_tools of this checkout (see "Which checkout" below)
"$FCN_VENV/bin/python" bench/<name>.py --help
"$FCN_VENV/bin/python" bench/<name>.py                     # the final measurement, into results/
"$FCN_VENV/bin/python" bench/<name>.py --quick --out /tmp/b # a test of the bench, seconds
"$FCN_VENV/bin/python" bench/<name>.py --replot            # redraw results/<name>.png from the JSON
```

Every bench of phase 6 has the options of `fcn_tools.results.add_bench_arguments()`:

| option | what it does |
|---|---|
| (none) | the documented default: the final measurement, written to `results/` |
| `--quick` | a short run (a few seconds to a minute) that tests the bench, not a measurement; needs `--out` |
| `--out DIR` | write `<name>.json` and `<name>.png` into DIR instead of `results/` |
| `--replot` | only redraw `<name>.png` from `<name>.json` of the output directory |
| `--max-load L` | refuse to measure when the 1-minute load average at the start is above L (default 1.0) |
| `--allow-loaded` | measure anyway; the result then says `"loaded": true` |
| `--no-build` | do not run `scripts/build.sh release` first; measure the release build as it is |

and its own options for the parameters that shape its numbers (durations, trial counts, seeds,
bit rates, transports), each recorded in the result's `config`.

**Which checkout.** A result names the commit of the checkout `fcn_tools` is imported from, and
the bench script computes its figures (isotp_throughput.py and updates.py keep their analysis
functions in it). A bench therefore refuses to measure (exit status 4) unless its own script is
the file at the same path in that checkout, byte for byte (`fcn_tools.results.script_mismatch()`;
the bytes are compared, not the paths, because WSL reaches one tree through more than one
mount). The editable install of scripts/setup-python.sh points the virtual environment at the
one checkout it was made from: a bench of another checkout, run without `PYTHONPATH` set to its
own `tools/`, imports that checkout's fcn_tools and is refused, where it would otherwise have
written into that checkout's results/ under its commit. run_all.py refuses the same for itself.

**Which binaries.** A bench builds the release preset first (`scripts/build.sh release`) unless
it is told which binaries to use: `--no-build` (the release build as it is), or the environment
variables `FCN_NODE`, `FCN_BOOT` and `FCN_FLASH_TOOL` (fcn_tools/paths.py). Whatever it runs, it
writes a result only if every binary carries the build id of the tree the result names: the
commit, and for a tree with uncommitted changes a hash of those changes
(platform/posix/build_id.h; the node reports it in `--version`, in its log and in its statistics
as `"build_id"`, fcn_boot in `--version` and its log, fcn_flash in `--version`). The submodules
count (third_party/): a kernel checked out at another commit than the tree records, or with a
tracked file edited, is a change; an untracked file is none, in the tree or in a submodule.
`fcn_tools.results.require_tree_build()` checks the binaries and the script before the bench
measures, and `write_result()` again when it writes.

**An idle machine.** Timing benches assume that nothing else runs. The 1-minute load average is
taken when the bench starts (before its build) and recorded with the one at the end in the
result's provenance; above `--max-load` (default 1.0: this WSL2 VM of 8 logical CPUs shows 0.0 to
0.3 idle, a build or a test run 2 to 8, one busy loop adds 1) the bench refuses, unless
`--allow-loaded` makes it measure and mark the result `"loaded": true`. A loaded result is not
quoted.

**Exit status.** 0 a result was written; 1 an error (the bench says what); 2 a usage error (a
`--quick` run without `--out` among them); 3 the host was loaded (`EXIT_HOST_LOADED`); 4 a binary
is not the tree's build, or the bench script is not the tree's (`EXIT_BUILD_MISMATCH`); 128 + N
ended by signal N (Ctrl-C: 130). No result file is written or changed unless the status is 0,
with one exception: power_loss.py and updates.py write their result and then exit with 5
(`EXIT_FINDINGS`) when a trial failed a check or an update or a download did not succeed,
because a failure is a finding that the result must keep (power_loss's `failures`, updates'
records), and run_all.py stops there until someone has looked at it. (platform_timing.py, from
before phase 6, has `--seconds`, `--replot`, `--out`,
`--max-load` and `--allow-loaded` with exit status 3 for a loaded host, but no `--quick` and no
`--no-build`: it builds unless `FCN_SELFTEST` names a self-test, checks the self-test's build id
and its own script the same way, after it has measured, and exits 1 for those and every other
refusal.)

## What a result holds

`fcn_tools.results.write_result(name, data, summary, config, out_dir, ...)` writes, in this order:

| member | |
|---|---|
| `schema` | `fcn.result/<name>/<version>` |
| `provenance` | command, commit, modified files, host, load averages at the start and the end |
| `host_timing_note` | `HOST_TIMING_NOTE`, and the bench's own sentence where it says more or less |
| `loaded`, `load_limit` | the load check (above) |
| `build` | the tree's build id, each binary's path, build id and SHA-256, the ids the runs reported |
| `config` | every parameter that shapes the numbers |
| `summary` | the figures the README may quote, each with its unit (`figure()`, `distribution()`) |
| the data | the bench's own members |

**Distributions** (`distribution(samples, unit)`) are given as `n`, `min`, `p50`, `p90`, `p99`,
`p99_9`, `max`, `mean` and `stdev` (the sample's, n - 1), with their unit. A percentile p is given
only when at least five samples lie beyond it, n (1 - p) >= 5: p50 needs n >= 10, p90 n >= 50,
p99 n >= 500 and p99.9 n >= 5000; below that it is `null`. The standard deviation needs n >= 2.
`raw_samples()` keeps the samples themselves up to 5000 of them and a histogram of 200 equal
bins above that. Every figure is computed by a function that tests/python tests on constructed
data.

## Writing a bench

The shared pieces, all in tools/fcn_tools (no pytest in any of them):

| module | |
|---|---|
| `results` | the result skeleton, `distribution()`, `raw_samples()`, `figure()`, the build-id checks, the load check, `add_bench_arguments()`, `output_dir()` |
| `runner` | `prepare_binaries()` (build or take the binaries), `Session` (nodes, Boards, ports, the SocketCAN lock, statistics at the end, cleanup on every exit path), `bench_workdir()` |
| `bench` | the flash tool (`factory`, `stage`, `info`), `make_images()` and `pack_image()`, the frames (`decoded`, `listen`), the waits that follow a trial, `Flood`, `TelemetryOff` |
| `update_model` | the reference: `expected_boot()`, `check_invariants()`, written from docs/update.md |
| `tester` | the diagnostic tester: `RawLink`, `Uds`, `download()`, `run_flasher()` |
| `capture` | an observer of the bus in a process of its own (`Observer`, `python -m fcn_tools.capture`): every frame with the kernel's receive timestamp, put on CLOCK_MONOTONIC frame by frame across the wall clock's steps (`to_monotonic()`), and the windows of a run (`steady_window()`, `exchange_window()`) |
| `trace` | the node's host-only trace (`fcn_node --trace-file`, platform/posix/posix_trace.h): the tick's grid, running intervals, jobs, the frames the node completed, the samples |
| `latency` | node_timing's analysis: the sample of a frame by its E2E counter, latencies, the observer against the node, periods, CPU shares, stack use |
| `busload` | bus_load's analysis: exact frame lengths, load per identifier and in all, the DBC's nominal load |
| `busoff` | bus_off's analysis: the standard's, the model's and the policy's constants, the intervals of a trial from the controller trace (`simctl.SimControl.can_trace()`), the node's log and the observer, the run's count checks |
| `powerloss` | power_loss's model and figures: the sequences of flash operations and what each leaves, the plan, the FATAL line of a cut, the start a state calls for, the rows and the summary |
| `plotstyle` | the look of every plot |

(isotp_throughput.py and updates.py keep their analysis functions in the script itself, and
tests/python/test_isotp_throughput_analysis.py and test_updates_analysis.py import them from
there. A unit-test file must not be called `test_bench_<name>.py`, the name of the bench's system
test: pytest refuses two test files of the same basename.)

The outline of a bench's `main()`:

```python
parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
results.add_bench_arguments(parser, quick="2 trials of 1 s")
args = parser.parse_args()
out = results.output_dir(args)
if args.replot:
    plot(json.loads((out / "name.json").read_text()), out / "name.png")
    return 0
try:
    load = results.check_load(args.max_load, args.allow_loaded)
    binaries = runner.prepare_binaries(build=not args.no_build)
    results.require_tree_build(binaries.as_dict())        # refuse before measuring for an hour
    with runner.Session(runner.bench_workdir("name"), "socketcan", binaries=binaries) as session:
        data, reported = measure(session, args)          # reported: {"run 1": stats["build_id"], ...}
    path = results.write_result("name", data, summary(data), config(args), out, load=load,
                                binaries=binaries.as_dict(), reported_build_ids=reported,
                                binary_sha256=binaries.sha256, timing_note="...")
except results.HostLoaded as error:
    print(error, file=sys.stderr)
    return results.EXIT_HOST_LOADED
except results.BuildIdMismatch as error:
    print(f"no result written: {error}", file=sys.stderr)
    return results.EXIT_BUILD_MISMATCH
except KeyboardInterrupt as error:                       # Ctrl-C, SIGTERM, SIGHUP
    return runner.interrupted_status(error)
plot(json.loads(path.read_text()), path.with_suffix(".png"))
```

Each bench also gets unit tests of its analysis in tests/python (constructed inputs with known
answers) and one functional system test `tests/system/test_bench_<name>.py`, which runs it with
`--quick --out <tmp> --allow-loaded --no-build` (the test machine is loaded by the test run
itself) and checks the JSON's schema and the consistency of its counts, never a timing figure.
Its bound on the run is generous (300 s, for a slow host), and the file's `pytestmark =
pytest.mark.timeout(...)` lies above it, so that pyproject.toml's 120 s per test does not end a
slow host's quick run first, without the bench's output.

## The benches

| bench | measures | command | default run |
|---|---|---|---|
| node_timing | release jitter of the periodic tasks, latency from a sample to the bus and to an observer, periods of the cyclic frames on the bus, CPU load per task, stack high-water marks; idle bus, a flood at half the bus, during a download; both transports; the node's host-only trace (`--trace-file`) and an observer with kernel timestamps (`python -m fcn_tools.capture`) | `python bench/node_timing.py` | per transport 120 s idle, 120 s flood, 3 downloads, 60 s without the trace; about 13 minutes |
| bus_load | every frame on the bus with its exact stuffed length: load per identifier and in all at 500 kbit/s, against the DBC's nominal load (worst-case and typical stuffing); during a download the tester's and the node's diagnostic frames apart; the node's model's busy time; both transports | `python bench/bus_load.py` | per transport 60 s idle and 1 download; about 3 minutes |
| bus_off | the way into bus off (TEC attempt by attempt) and the recovery: the recovery sequence against ISO 11898-1's 1408 bit times, the hold-off, what the host adds, the first frame, the frames lost, the DTCs; a short and 32 destroyed transmissions, at 500, 125 and 1000 kbit/s | `python bench/bus_off.py` | 100 trials of each fault per bit rate (5.35 s apart: the node's 5 s stable time) and a short held through 7 bus offs per bit rate, about 54 minutes |
| isotp_throughput | TransferData throughput by block size (1 to 1024 data bytes) from the tester to the node, each block cut into the turns of both sides on one clock with the node's write jobs and its event budget; the node's longest response (19 0A, 71 bytes) to the tester; a labelled projection for blocks of 2048 bytes | `python bench/isotp_throughput.py` | both transports, 10 block sizes of 64 blocks, 500 responses each, about 1 minute |
| updates | the update over the bus end to end with the flasher, each from the image the one before confirmed, step by step; the download of an image with a payload of 256 KiB | `python bench/updates.py` | 50 updates over SocketCAN, 10 over UDP, 10 downloads of 256 KiB, about 30 minutes; exit status 5 when an update or a download did not succeed (the result is written, with its records) |
| power_loss | power cuts in every flash operation of a staging, a download over the bus and the boot of a trial (its attempts, its confirm, its rollback), each followed by the restart through fcn_boot (checked against fcn_tools.update_model) and the next update; `--replay CLASS:SEQUENCE:OP:SEED` runs one trial again | `python bench/power_loss.py` | 1062 trials in 8 parallel workers, about 27 minutes; exit status 5 when a trial failed a check (the result is written, with the failures) |
| platform_timing | tick accuracy (stock and locked), simulated interrupt latency, interrupt storm | `python bench/platform_timing.py` | 4 runs of 10 s and 2 storms of 5 s, about a minute |

**The durations** come from the verification's rehearsal of phase 6 (2026-11-22 and 23, the
machine otherwise idle, each bench started at a 1-minute load average of at most 1.0, its wall
time on /proc/uptime, on the tree of the verification, which differs from the committed one only
by its uncommitted state). Three benches were run at their default and the times are measured:
bus_load 178 s and 171 s (two runs), isotp_throughput 53 s, platform_timing 52 s. The others were
run at about a fifth of the default, and the default is extrapolated from what each trial or run
took there; these are estimates:

| bench | rehearsal (command, wall time) | per trial or run, measured there | default, estimated |
|---|---|---|---|
| node_timing | `--seconds 24 --downloads 1 --control-seconds 12`, 179 s and 173 s | the node runs took their tick time (24 s, 12 s), a download run 24.6 to 25.8 s; 8 s for the rest | per transport 120 + 120 + 60 s and 3 downloads of about 25 s, 750 s, and the rest: about 13 minutes |
| bus_off | `--trials 20`, 681 s | trials 5.35 s apart (5.32 to 5.47), about 13 s per bit rate for the node's start, the persisting short and the end | 3 × (200 × 5.35 s + 13 s): about 54 minutes |
| updates | `--socketcan-updates 10 --udp-updates 2 --big-downloads 2`, 359 s | about 25.6 s per update or download of 256 KiB (24.4 to 26.4 s of flasher steps each) | 60 updates and 10 downloads: about 30 minutes |
| power_loss | `--stage-repeats 2 --download-repeats 1 --boot-repeats 7`, 402 s (248 trials) | trials of 6.2 s (stage), 27.5 s (download), 9.7 s (boot) on average, 7.7 of the 8 workers busy, about 18 s of preparation | 12,450 s of trials over 7.7 workers: about 27 minutes |

All of them in run_all.py's order: about 2 hours 10 minutes, plus the waits for the load to fall
back to 1.0 between benches: about 5 minutes after power_loss, whose 8 workers leave a load of
about 5, and little after the others. --quick runs took 4.5 s (isotp_throughput) to 33 s
(node_timing) each. The medium power_loss run ended with exit status 5: one trial's next update
was reset by the node's watchdog under the campaign's own load, and its replay passed
(docs/measurements.md, power_loss).

The final run, `python bench/run_all.py` on commit 28d8f7a (a clean tree) from 18:36:09 to
20:51:33 EST on 2026-11-23, 2 hours 15 minutes in all, took by run_all's own summary (each
bench's time includes its build and setup): node_timing 12.7 minutes, bus_load 3.0, bus_off
54.1, isotp_throughput 1.0, updates 29.1, power_loss 26.1 and platform_timing 0.9, each with
exit status 0; before platform_timing it waited 60 s for the load average to fall to 1.0 after
power_loss. Windows logged no standby from 18:30 to the end of the run. The results are
committed as 2d93e3e; results/power_loss.json holds no failure (`failures` is empty, 1062
trials).

## The final runs

The lead runs every bench on the committed tree, alone on the machine, so that every result's
provenance names a commit and no other work shares the host:

1. Commit everything; nothing else runs (`uptime` shows a load average below 1.0, `ps` no
   leftover node, test run or busy loop); `scripts/vcan-up.sh` has made vcan0.
2. `python bench/run_all.py` (`--list` shows the plan). It refuses to run from another checkout
   than the one fcn_tools is imported from (exit status 4) and a tree with uncommitted changes,
   the submodules' included, builds the release preset once, and runs the benches one at a time
   in the order below, each once the load average is at or below 1.0 again (the previous
   bench's load takes a minute or two to decay; it waits at most 15 minutes), stopping at the
   first bench that fails or reports a finding (exit status 5: look at the result before going
   on; `--from NAME` resumes at a bench).
3. Commit results/ with a message naming the commit the results were measured on.

The order (run_all.py's `PLAN`): the benches whose figures are host timing first, when the
machine has been idle longest, the longest of them later, and last the power-loss campaign, which
runs parallel workers and whose checks read no clock of the host's (a host stall can still fail
a trial, as the rehearsal above showed); platform_timing.py, the measurement of the host
mechanisms the others rest on, closes the run:

1. node_timing (latency, jitter, CPU load, stack high-water marks)
2. bus_load
3. bus_off (bus-off recovery)
4. isotp_throughput
5. updates (50 end-to-end updates, the 256 KiB image)
6. power_loss (the power-loss campaign, at least 1000 trials)
7. platform_timing
