"""Write measurement results with enough provenance to trust and to reproduce them.

Every number quoted in the README comes from a file written here: results/<name>.json holds
the data together with the command that produced it, the commit it was produced from and the
machine it ran on.

THE SKELETON. write_result() writes <name>.json with, in this order: "schema"
("fcn.result/<name>/<version>"), "provenance" (provenance(): command, commit, modified files,
host, load averages at the start and at the end), "host_timing_note" (HOST_TIMING_NOTE and a
sentence of the bench's own), then for a bench of phase 6 "loaded" and "load_limit" (see A
LOADED HOST), "build" (the build ids, see THE BUILD ID), "config" (every parameter that shapes the
numbers), "summary" (the figures the README may quote, each with its unit: check_summary()) and
the bench's own data. The file is written whole or not at all (a temporary file renamed over
it).

DISTRIBUTIONS. distribution() gives n, min, p50, p90, p99, p99.9, max, mean and the sample
standard deviation of a sample, with its unit. A percentile p is given only when at least
MIN_BEYOND (5) samples lie beyond it, n * (1 - p) >= 5: p50 needs n >= 10, p90 n >= 50, p99
n >= 500 and p99.9 n >= 5000; below that it is null (None), because with fewer samples beyond it
the "percentile" is one of the largest few values seen, as uncertain as the maximum itself. The
standard deviation needs n >= 2. percentiles(), the older summary of platform_timing.py, follows
the same rule. raw_samples() keeps the samples themselves in the JSON up to RAW_SAMPLES_CAP
(5000) values and a histogram of HISTOGRAM_BINS (200) equal bins above that.

THE BUILD ID. Every host program carries the build id of the tree it was built from
(platform/posix/build_id.h, cmake/FcnBuildId.cmake): the commit, and "-dirty-" with a hash of
the uncommitted changes for a tree that has them, in the submodules as well (a submodule moved
to another commit, or a tracked file changed in one). tree_build_id() computes the id a binary
built from the tree of a provenance carries, by the same rule; binary_build_id() asks a program
(`<program> --version`); require_tree_build() refuses (BuildIdMismatch) a set of binaries, or of
ids they reported (a node's statistics have "build_id"), of which one is not the tree's.
write_result() checks again just before and just after it writes, against the provenance it
writes, and puts the previous file back if the tree changed in between.

THE SCRIPT. A result names the commit of paths.REPO_ROOT, the checkout this package is imported
from, and its command names the bench script by its path in its own checkout; the script
computes the figures. script_mismatch() says why the script running is not the file at that
path in REPO_ROOT, byte for byte: a script of another checkout (the editable install of
scripts/setup-python.sh points the virtual environment at one checkout) or of none.
require_tree_build() refuses such a script before the bench measures, write_result() when it
writes, and build_id_mismatch() for platform_timing.py, so that no result names code that did
not compute it.

A LOADED HOST. Timing benches assume an otherwise idle machine. check_load() compares the
1-minute load average at the start of the bench (taken when this module is imported, before the
build and before anything is measured: provenance's load_average_at_start) with a limit
(DEFAULT_LOAD_LIMIT, 1.0, or --max-load) and refuses (HostLoaded) above it, unless the bench was
told to measure anyway (--allow-loaded): then the result says "loaded": true.

THE COMMAND LINE OF A BENCH. add_bench_arguments() adds the options every bench has: --quick,
--out DIR, --replot, --max-load, --allow-loaded and --no-build; output_dir() says where the
files go. A --quick run is a test of the bench: it needs --out, so that results/ only ever holds
full runs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Mapping

import numpy as np

from . import paths


# Travels with every result, because a number that left its README is still read by someone.
HOST_TIMING_NOTE = (
    "Measured on the FreeRTOS POSIX port, where every task is a thread of a Linux process. "
    "Times, jitter and latencies are those of the host's scheduler and are not what the same "
    "code shows on a microcontroller. The contents, orderings and sizes of protocol data, and "
    "the frames and bytes an exchange takes, do not depend on the host. What happens in a "
    "stretch of time does: the jobs and frames of a run of fixed length, ticks dropped, jobs "
    "released early; and a stall of the host past the node's 500 ms watchdog or a 1 s protocol "
    "timer fails what the node would have passed. Such a failure is never hidden (the bench "
    "counts and keeps it, or writes no result), and no stall makes a check pass, so a count of "
    "0 failures holds whatever the host did."
)


def _load_average() -> float | None:
    """The host's 1-minute load average, or None where the system does not report one."""
    try:
        return round(os.getloadavg()[0], 2)
    except (AttributeError, OSError):
        return None


# Taken when a measurement script imports this module, that is before it measures anything.
_LOAD_AT_START = _load_average()


def _git(*args: str) -> str:
    """Output of a git command in the repository, without the trailing newline; "" on error."""
    try:
        return subprocess.run(
            ["git", "-C", str(paths.REPO_ROOT), *args],
            capture_output=True, text=True, timeout=60, check=True,
        ).stdout.rstrip("\n")
    except (OSError, subprocess.SubprocessError):
        return ""


def _modified_files(porcelain: str) -> list[str]:
    """Paths from `git status --porcelain` output, without results/ (rewritten by every run).

    Each line is two status characters, a space and the path. The first status character is
    often a space, so the text must not be stripped before it is cut."""
    paths_ = [line[3:] for line in porcelain.splitlines() if len(line) > 3]
    return [path for path in paths_ if not path.startswith("results/")]


def _cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def _checkout_of(script: Path) -> tuple[Path | None, str]:
    """The checkout `script` (an absolute path) lies in, the nearest directory above it with a
    CMakePresets.json, and the script's path in it; (None, its file name) outside every
    checkout."""
    for parent in script.parents:
        if (parent / "CMakePresets.json").exists():
            return parent, script.relative_to(parent).as_posix()
    return None, script.name


def provenance() -> dict:
    """Who produced a result: command, commit, uncommitted source changes, host."""
    modified = _modified_files(_git(*STATUS_FOR_BUILD_ID))
    # The script's path relative to the checkout it lives in, found by walking up from the
    # script itself (the package may be imported through a different mount of the same tree).
    _, script_name = _checkout_of(Path(sys.argv[0]).resolve())
    release = platform.release()
    return {
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "command": " ".join(["python", script_name, *sys.argv[1:]]),
        "git_commit": _git("rev-parse", "HEAD") or "unknown",
        "git_modified_files": modified,
        "host": {
            "kernel": release,
            "is_wsl": "microsoft" in release.lower(),
            "machine": platform.machine(),
            "cpu": _cpu_model(),
            "logical_cpus": os.cpu_count(),
            "python": platform.python_version(),
            # What else the machine was doing: the 1-minute load average before the first
            # measurement and after the last. A timing result from a loaded host says so.
            "load_average_at_start": _LOAD_AT_START,
            "load_average_at_end": _load_average(),
        },
    }


# ------------------------------------------------------------------------------ distributions

#: A percentile is given only with at least this many samples beyond it (see the module text).
MIN_BEYOND = 5

#: The percentiles of distribution() and percentiles(): their keys and their values in percent.
PERCENTILES = (("p50", 50.0), ("p90", 90.0), ("p99", 99.0), ("p99_9", 99.9))

#: raw_samples() keeps at most this many samples in a result; above it, a histogram.
RAW_SAMPLES_CAP = 5000
HISTOGRAM_BINS = 200


def smallest_n(percent: float) -> int:
    """The smallest sample size for which the percentile `percent` is given: n * (1 - p) >= 5,
    computed in whole tenths of a percent so that the boundary is exact (in binary floating
    point 50 * (1 - 0.9) is 4.999...)."""
    tenths = round(percent * 10)
    return -(-MIN_BEYOND * 1000 // (1000 - tenths))


def _percentile(values: np.ndarray, percent: float) -> float | None:
    if values.size < smallest_n(percent):
        return None
    return float(np.percentile(values, percent))


def _rounded(value: float | None, decimals: int) -> float | None:
    return None if value is None else round(value, decimals)


def percentiles(samples, unit_divisor: float = 1.0) -> dict:
    """Summary of a latency-like sample: count, mean, min, percentiles and max.

    A percentile p is only reported when the sample holds at least five values beyond it,
    n * (1 - p) >= 5, so p99 needs 500 samples and p99.9 needs 5,000. Below that the entry is
    None: with fewer samples the "percentile" is just the largest value seen, and a number
    the data cannot support is worse than no number. The count is always reported.
    (bench/platform_timing.py's summary; the benches of phase 6 use distribution(), which adds
    the standard deviation and the unit.)
    """
    values = np.asarray(samples, dtype=np.float64) / unit_divisor
    n = int(values.size)
    if n == 0:
        return {"n": 0}
    summary: dict = {"n": n, "mean": round(float(values.mean()), 3),
                     "min": round(float(values.min()), 3)}
    for key, percent in PERCENTILES:
        summary[key] = _rounded(_percentile(values, percent), 3)
    summary["max"] = round(float(values.max()), 3)
    return summary


def distribution(samples: Iterable[float], unit: str, *, scale: float = 1.0,
                 decimals: int = 3) -> dict:
    """The distribution of a sample, in `unit`: {"unit", "n", "min", "p50", "p90", "p99",
    "p99_9", "max", "mean", "stdev"}. Each sample is multiplied by `scale` first (1e-3 for
    nanoseconds given in microseconds, say) and every figure is rounded to `decimals` places.
    A percentile without MIN_BEYOND samples beyond it is None (see the module text), the
    standard deviation (the sample's, with n - 1) is None below two samples, and an empty sample
    has n 0 and None everywhere else. NaN or an infinite sample is refused (ValueError): a
    measurement that produced one has a bug the distribution must not hide."""
    values = np.asarray(list(samples) if not isinstance(samples, np.ndarray) else samples,
                        dtype=np.float64) * scale
    if values.ndim != 1:
        values = values.reshape(-1)
    if not np.all(np.isfinite(values)):
        raise ValueError(f"a sample in {unit} is not a finite number")
    n = int(values.size)
    result: dict = {"unit": unit, "n": n}
    if n == 0:
        result.update({"min": None, **{key: None for key, _ in PERCENTILES}, "max": None,
                       "mean": None, "stdev": None})
        return result
    result["min"] = _rounded(float(values.min()), decimals)
    for key, percent in PERCENTILES:
        result[key] = _rounded(_percentile(values, percent), decimals)
    result["max"] = _rounded(float(values.max()), decimals)
    result["mean"] = _rounded(float(values.mean()), decimals)
    result["stdev"] = _rounded(float(values.std(ddof=1)), decimals) if n >= 2 else None
    return result


def raw_samples(samples: Iterable[float], unit: str, *, scale: float = 1.0, decimals: int = 3,
                cap: int = RAW_SAMPLES_CAP, bins: int = HISTOGRAM_BINS) -> dict:
    """The samples themselves for a result's JSON, in `unit` (multiplied by `scale`, rounded to
    `decimals`), in the order given: {"unit", "n", "samples": [...]} for at most `cap` samples;
    above that {"unit", "n", "histogram": {"edges": [bins + 1 values], "counts": [bins counts]}}
    with `bins` equal bins from the smallest to the largest sample (numpy.histogram: every bin
    holds its left edge, the last one its right edge as well, so the counts add up to n)."""
    values = np.asarray(list(samples) if not isinstance(samples, np.ndarray) else samples,
                        dtype=np.float64).reshape(-1) * scale
    if not np.all(np.isfinite(values)):
        raise ValueError(f"a sample in {unit} is not a finite number")
    n = int(values.size)
    if n <= cap:
        return {"unit": unit, "n": n, "samples": [round(float(v), decimals) for v in values]}
    counts, edges = np.histogram(values, bins=bins)
    return {"unit": unit, "n": n, "cap": cap,
            "histogram": {"edges": [round(float(e), decimals) for e in edges],
                          "counts": [int(c) for c in counts]}}


def figure(value: float | int | None, unit: str) -> dict:
    """One figure of a summary: {"value": value, "unit": unit}."""
    if isinstance(value, float) and not math.isfinite(value):
        raise ValueError(f"a figure in {unit} is not a finite number")
    return {"value": value, "unit": unit}


def check_summary(summary: Mapping, where: str = "summary") -> None:
    """Every figure of a summary has its unit: each entry is a figure (figure()) or a
    distribution (distribution()), both of which carry "unit", or a group, a mapping of such
    entries. ValueError names the first entry that has none."""
    if not isinstance(summary, Mapping):
        raise ValueError(f"{where} is not a mapping")
    for key, entry in summary.items():
        name = f"{where}.{key}"
        if not isinstance(entry, Mapping):
            raise ValueError(f"{name} has no unit: give it as figure(value, unit)")
        if "unit" in entry:
            if not isinstance(entry["unit"], str) or not entry["unit"]:
                raise ValueError(f"{name}: its unit is not a word")
            continue
        if not entry:
            raise ValueError(f"{name} is empty")
        check_summary(entry, name)


# ------------------------------------------------------------------------------- the build id

# Whether the tree is dirty (the files of the provenance), and the changes that make up a dirty
# build id; both must match cmake/FcnBuildId.cmake exactly. A submodule moved to another commit,
# or with a tracked file changed in it, is a change: the status lists it, and --submodule=diff
# puts its own diff into the hash. Untracked files are none, in the tree or in a submodule.
STATUS_FOR_BUILD_ID = ["status", "--porcelain", "--untracked-files=no",
                       "--ignore-submodules=untracked"]
DIFF_FOR_BUILD_ID = ["diff", "HEAD", "--binary", "--no-color", "--no-ext-diff", "--no-textconv",
                     "--no-renames", "--submodule=diff", "--ignore-submodules=untracked", "--",
                     ".", ":(exclude)results"]

#: What a build id looks like: the commit (40 hex digits, 64 in a SHA-256 repository), with
#: "-dirty-" and 12 hex digits for a tree with uncommitted changes; or "unknown".
BUILD_ID = re.compile(r"^(unknown|[0-9a-f]{40}([0-9a-f]{24})?(-dirty-[0-9a-f]{12})?)$")


def changes_hash(repo: Path | None = None) -> str:
    """First 12 hex digits of the git blob hash of the tree's uncommitted changes, computed as
    cmake/FcnBuildId.cmake computes it; "unknown" if git fails. `repo`: the repository (default
    this one)."""
    git = ["git", "-C", str(paths.REPO_ROOT if repo is None else repo), "--no-optional-locks"]
    try:
        diff = subprocess.run([*git, *DIFF_FOR_BUILD_ID], capture_output=True, check=True)
        blob = subprocess.run([*git, "hash-object", "--stdin"], input=diff.stdout,
                              capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return "unknown"
    digest = blob.stdout.decode().strip()
    return digest[:12] if digest else "unknown"


def tree_build_id(provenance: dict, repo: Path | None = None) -> str:
    """The build id that a binary built from the tree in `provenance` carries.

    Same rule as cmake/FcnBuildId.cmake: the commit; if tracked files other than results/
    differ from it, "-dirty-" and the hash of the changes as they are now (changes_hash);
    "unknown" without git. `repo`: the repository the provenance describes (default this
    one)."""
    commit = provenance["git_commit"]
    if commit == "unknown":
        return "unknown"
    if not provenance["git_modified_files"]:
        return commit
    digest = changes_hash(repo)
    return "unknown" if digest == "unknown" else f"{commit}-dirty-{digest}"


def script_mismatch(script: Path | None = None) -> str | None:
    """Why the bench script `script` (default the one running, sys.argv[0], as provenance()
    names it) is not this checkout's own, or None if it is (see THE SCRIPT in the module text).

    It must be the file at its path in its own checkout under paths.REPO_ROOT, with the same
    bytes. The bytes are compared, not the paths: under WSL the same tree is reached through
    more than one mount (/home/dev/fcn and /mnt/c/...), and a copy with the same bytes computes
    the same figures, as a bench imports nothing from its own directory."""
    path = Path(sys.argv[0] if script is None else script).resolve()
    root, relative = _checkout_of(path)
    if root is None:
        return (f"the bench script {path} lies in no checkout of this project, so a result "
                f"could not name the code that computed it: run the bench of {paths.REPO_ROOT}")
    own = paths.REPO_ROOT / relative
    try:
        same = own.is_file() and own.read_bytes() == path.read_bytes()
    except OSError:
        same = False
    if not same:
        return (f"the bench script {path} is not {own}: fcn_tools is imported from "
                f"{paths.REPO_ROOT}, whose commit a result would name, but the figures would be "
                f"computed by the script of {root}. Run that checkout's bench with "
                f"PYTHONPATH={root / 'tools'}, or the bench of {paths.REPO_ROOT}")
    return None


def build_id_mismatch(build_ids: set[str], provenance: dict,
                      reporter: str = "the self-test") -> str | None:
    """Why the result would not name what made it, or None: the bench script running is not
    this checkout's (script_mismatch()), or the binary does not belong to the tree in
    `provenance`. `build_ids` are the ids `reporter` reported (bench/platform_timing.py: the
    self-test)."""
    script = script_mismatch()
    if script is not None:
        return script
    expected = tree_build_id(provenance)
    if expected == "unknown":
        return "the tree's build id is unknown (no git?), so the binary cannot be tied to it"
    if build_ids != {expected}:
        return (f"{reporter} reports build id {', '.join(sorted(build_ids))}, but the tree is "
                f"{expected}: the binary was not built from the tree the result would name")
    return None


class BuildIdMismatch(RuntimeError):
    """A binary a bench used was not built from the tree its result would name."""


#: `<program> --version` ends its line with "build <id>" (node_main.c, boot_main.c,
#: flash_tool.c).
_VERSION_BUILD = re.compile(r"\bbuild (\S+)\s*$")


def binary_build_id(binary: Path) -> str:
    """The build id `binary` (fcn_node, fcn_boot or fcn_flash) reports: the last word of the
    first line of `binary --version`, after "build". BuildIdMismatch if the program does not
    run, does not say, or says something that is no build id."""
    try:
        run = subprocess.run([str(binary), "--version"], capture_output=True, text=True,
                             timeout=30, check=False, stdin=subprocess.DEVNULL)
    except OSError as error:
        raise BuildIdMismatch(f"{binary} cannot be run to ask its build id: {error}") from error
    first = run.stdout.splitlines()[0] if run.stdout else ""
    found = _VERSION_BUILD.search(first)
    if run.returncode != 0 or found is None or not BUILD_ID.match(found.group(1)):
        raise BuildIdMismatch(f"{binary} --version does not report a build id (status "
                              f"{run.returncode}, output {run.stdout.strip()!r}): it is no "
                              "build of this tree's programs, or one from before phase 6")
    return found.group(1)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def binaries_mismatch(binaries: Mapping[str, Path], provenance: dict,
                      reported: Mapping[str, str] | None = None) -> str | None:
    """Why one of `binaries` (name -> path, each asked with binary_build_id()) or of the build
    ids `reported` by what ran (name -> id, for example {"node statistics": stats["build_id"]})
    does not belong to the tree of `provenance`, naming each one that does not; None if all
    do."""
    expected = tree_build_id(provenance)
    if expected == "unknown":
        return "the tree's build id is unknown (no git?), so no binary can be tied to it"
    wrong = []
    for name, binary in binaries.items():
        try:
            found = binary_build_id(binary)
        except BuildIdMismatch as error:
            wrong.append(str(error))
            continue
        if found != expected:
            wrong.append(f"{name} ({binary}) is build {found}")
    for name, found in (reported or {}).items():
        if found != expected:
            wrong.append(f"{name} reports build {found}")
    if not wrong:
        return None
    return (f"the tree is build {expected}, but " + "; ".join(wrong) + ". The result would name "
            "a tree its binaries were not built from: build them from this tree "
            "(scripts/build.sh release), or commit or undo the changes made since")


def require_tree_build(binaries: Mapping[str, Path], reported: Mapping[str, str] | None = None,
                       provenance_: dict | None = None) -> str:
    """Refuse (BuildIdMismatch, with script_mismatch()'s or binaries_mismatch()'s message)
    unless the bench script running is this checkout's and every binary and every reported id
    is the build of the tree (of `provenance_`, default the tree as it is now); the tree's build
    id otherwise. A bench calls it before it measures, so that it does not measure for an hour
    what it would not write, and write_result() again when it writes."""
    prov = provenance() if provenance_ is None else provenance_
    mismatch = script_mismatch() or binaries_mismatch(binaries, prov, reported)
    if mismatch is not None:
        raise BuildIdMismatch(mismatch)
    return tree_build_id(prov)


# -------------------------------------------------------------------------------- a loaded host

#: The 1-minute load average above which a bench refuses to start (check_load()). The VM this
#: project is measured on (WSL2, 8 logical CPUs) shows 0.0 to 0.3 when idle; a build or a test
#: run of the project shows 2 to 8, and a single busy loop adds 1.
DEFAULT_LOAD_LIMIT = 1.0


class HostLoaded(RuntimeError):
    """The host was not idle when the bench started, and the bench was not told to go on."""


@dataclass(frozen=True)
class LoadCheck:
    """What check_load() found: the limit, the 1-minute load average at the start (None where
    the system has none), and whether the result is to be marked loaded."""
    limit: float
    load_at_start: float | None
    loaded: bool


def load_at_start() -> float | None:
    """The 1-minute load average when this module was imported: before the bench built or
    measured anything (provenance's load_average_at_start)."""
    return _LOAD_AT_START


def check_load(limit: float = DEFAULT_LOAD_LIMIT, allow_loaded: bool = False,
               load: float | None = None) -> LoadCheck:
    """The load check of a timing bench: the 1-minute load average at the start (`load`, by
    default load_at_start()) against `limit`. Above it: HostLoaded, or, with `allow_loaded`, a
    LoadCheck whose `loaded` is True, which write_result() puts into the result. A system that
    reports no load average passes, unmarked: provenance then shows null."""
    if not limit > 0 or not math.isfinite(limit):
        raise ValueError(f"a load limit must be a positive number, not {limit}")
    value = load_at_start() if load is None else load
    over = value is not None and value > limit
    if over and not allow_loaded:
        raise HostLoaded(f"the host is not idle: its 1-minute load average was {value} at the "
                         f"start, above the limit {limit} (--max-load). Wait until nothing else "
                         "runs, or pass --allow-loaded to measure anyway: the result then says "
                         "\"loaded\": true")
    return LoadCheck(limit=limit, load_at_start=value, loaded=over)


# ----------------------------------------------------------------------- the command line

#: Exit statuses of a bench that wrote nothing on purpose (see bench/README.md).
EXIT_HOST_LOADED = 3
EXIT_BUILD_MISMATCH = 4


def add_bench_arguments(parser: argparse.ArgumentParser, *, quick: str) -> None:
    """The options every bench of phase 6 has; `quick` says what --quick does for this one."""
    parser.add_argument("--quick", action="store_true",
                        help=f"a test of the bench, not a measurement: {quick}. Needs --out")
    parser.add_argument("--out", type=Path, default=None, metavar="DIR",
                        help="write <name>.json and <name>.png into DIR (default: results/ of "
                             "the repository, for full runs only)")
    parser.add_argument("--replot", action="store_true",
                        help="only redraw <name>.png from <name>.json of the output directory")
    parser.add_argument("--max-load", type=float, default=DEFAULT_LOAD_LIMIT, metavar="L",
                        help="refuse to measure when the 1-minute load average at the start is "
                             f"above L (default {DEFAULT_LOAD_LIMIT})")
    parser.add_argument("--allow-loaded", action="store_true",
                        help="measure even above --max-load; the result then says "
                             "\"loaded\": true")
    parser.add_argument("--no-build", action="store_true",
                        help="do not run scripts/build.sh release first: measure the release "
                             "build as it is (FCN_NODE, FCN_BOOT and FCN_FLASH_TOOL name other "
                             "binaries); the build ids are checked all the same")


def output_dir(args: argparse.Namespace) -> Path:
    """Where a bench writes: --out, or results/ for a full run. A --quick run without --out is
    a usage error (SystemExit 2): results/ holds only the measurements the README quotes."""
    if args.out is not None:
        return Path(args.out)
    if getattr(args, "quick", False):
        print("a --quick run is a test of the bench: give --out DIR (results/ holds only full "
              "runs)", file=sys.stderr)
        raise SystemExit(2)
    return paths.results_dir()


# ------------------------------------------------------------------------------- the result

#: The members write_result() writes itself, which a bench's data must not use.
RESERVED = ("schema", "provenance", "host_timing_note", "loaded", "load_limit", "build", "config",
            "summary")


def _write_atomically(path: Path, payload: dict) -> None:
    """`payload` as JSON at `path`, whole or not at all: a temporary file renamed over it."""
    handle, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as out:
            json.dump(payload, out, indent=2)
            out.write("\n")
        os.chmod(temporary, 0o644)  # mkstemp's 0600 would make the result private
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def write_result(name: str, data: dict, summary: dict | None = None, config: dict | None = None,
                 out_dir: Path | None = None, *, version: int = 1, timing_note: str | None = None,
                 load: LoadCheck | None = None, binaries: Mapping[str, Path] | None = None,
                 reported_build_ids: Mapping[str, str] | None = None,
                 binary_sha256: Mapping[str, str] | None = None) -> Path:
    """Write <out_dir>/<name>.json (out_dir: results/ by default) and return its path; see the
    module text for the members and their order.

    `timing_note`: the bench's own sentence after HOST_TIMING_NOTE. `load`: check_load()'s
    verdict ("loaded" and "load_limit"). `binaries` (name -> path) and `reported_build_ids`
    (name -> id): what the bench ran; with either, the result is only written if all of them
    are the build of the tree the provenance names (BuildIdMismatch otherwise, and nothing is
    written), and "build" records the tree's id, each binary's path, id and SHA-256.
    `binary_sha256` (name -> digest taken before the bench measured): a binary that changed
    since is refused as well, and so is a bench script that is not this checkout's
    (require_tree_build()). `summary` must give every figure with its unit (check_summary()).
    Without these arguments the file is what bench/platform_timing.py has always written:
    schema, provenance, host timing note and the data."""
    clash = [key for key in data if key in RESERVED]
    if clash:
        raise ValueError(f"the data of {name} uses the reserved member(s) {', '.join(clash)}")
    if summary is not None:
        check_summary(summary)
    directory = paths.results_dir() if out_dir is None else Path(out_dir)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{name}.json"

    prov = provenance()
    checked = binaries is not None or reported_build_ids is not None
    build = None
    if checked:
        tree = require_tree_build(binaries or {}, reported_build_ids, prov)
        changed = [key for key, digest in (binary_sha256 or {}).items()
                   if sha256_of((binaries or {})[key]) != digest]
        if changed:
            raise BuildIdMismatch(f"{', '.join(changed)} changed while the bench measured: "
                                  "no result written")
        build = {"tree": tree,
                 "binaries": {key: {"path": str(binary), "build_id": tree,
                                    "sha256": sha256_of(binary)}
                              for key, binary in (binaries or {}).items()},
                 "reported": dict(reported_build_ids or {})}

    payload: dict = {"schema": f"fcn.result/{name}/{version}", "provenance": prov,
                     "host_timing_note": HOST_TIMING_NOTE if timing_note is None
                     else f"{HOST_TIMING_NOTE} {timing_note}"}
    if load is not None:
        payload["loaded"] = load.loaded
        payload["load_limit"] = load.limit
    if build is not None:
        payload["build"] = build
    if config is not None:
        payload["config"] = config
    if summary is not None:
        payload["summary"] = summary
    payload.update(data)

    previous = path.read_bytes() if checked and path.exists() else None
    _write_atomically(path, payload)
    if checked:
        # The tree may have changed in the moment between the check and the write: then put
        # back what was there, so that no result names a tree its binaries were not built from.
        mismatch = binaries_mismatch(binaries or {}, prov, reported_build_ids)
        if mismatch is not None:
            if previous is None:
                path.unlink()
            else:
                path.write_bytes(previous)
            raise BuildIdMismatch(mismatch)
    return path
