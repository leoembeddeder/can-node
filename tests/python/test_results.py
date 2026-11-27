"""The skeleton every result file uses (tools/fcn_tools/results.py): the summary statistics, the
build id of the tree and of the binaries, the check of the bench script, the load check, the
command line of a bench and write_result() itself. Every input is constructed, with a known
answer."""
from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

from fcn_tools import paths, results
from fcn_tools.results import distribution, percentiles


@pytest.fixture(autouse=True)
def _a_bench_of_this_checkout(monkeypatch):
    """The checks run in-process here, where the program running is pytest: each test stands
    for a bench of this checkout, the script that script_mismatch() checks and provenance()
    names (sys.argv[0]). The tests of the check itself plant other scripts."""
    monkeypatch.setattr(sys, "argv", [str(paths.REPO_ROOT / "bench" / "platform_timing.py")])


# ---------------------------------------------------------------------------- percentiles()


@pytest.mark.parametrize("key, smallest_n", [
    ("p50", 10), ("p90", 50), ("p99", 500), ("p99_9", 5000),
])
def test_a_percentile_is_reported_from_exactly_the_documented_sample_size(key, smallest_n):
    # n * (1 - p) >= 5: at the boundary it is reported, one sample fewer it is not.
    assert percentiles(range(smallest_n))[key] is not None
    assert percentiles(range(smallest_n - 1))[key] is None


def test_the_summary_of_a_known_sample():
    summary = percentiles([4.0, 1.0, 3.0, 2.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0])
    assert summary["n"] == 10
    assert summary["min"] == 1.0 and summary["max"] == 10.0
    assert summary["mean"] == 5.5
    assert summary["p50"] == 5.5
    assert summary["p90"] is None and summary["p99"] is None


def test_values_are_divided_by_the_unit_divisor():
    summary = percentiles([1000.0] * 10, unit_divisor=1000.0)
    assert summary["p50"] == 1.0 and summary["mean"] == 1.0


def test_an_empty_sample_has_only_its_count():
    assert percentiles([]) == {"n": 0}


def test_percentiles_keeps_its_members_and_their_order():
    # bench/platform_timing.py's JSON must not change: the same keys in the same order.
    assert list(percentiles(range(6000))) == ["n", "mean", "min", "p50", "p90", "p99", "p99_9",
                                              "max"]


# --------------------------------------------------------------------------- distribution()


@pytest.mark.parametrize("percent, n", [(50.0, 10), (90.0, 50), (99.0, 500), (99.9, 5000)])
def test_the_smallest_sample_of_each_percentile_leaves_five_beyond_it(percent, n):
    assert results.smallest_n(percent) == n
    assert math.isclose(n * (1 - percent / 100), 5.0)


@pytest.mark.parametrize("key, n", [("p50", 10), ("p90", 50), ("p99", 500), ("p99_9", 5000)])
def test_distribution_gives_a_percentile_from_exactly_its_smallest_sample(key, n):
    assert distribution(range(n), "us")[key] is not None
    assert distribution(range(n - 1), "us")[key] is None


def test_the_distribution_of_a_known_sample():
    # 1 to 1000: min 1, max 1000, mean 500.5, the sample standard deviation of 1..N is
    # sqrt(N (N + 1) / 12); numpy's linear percentile of 1..1000 at p is 1 + 999 p.
    got = distribution(range(1, 1001), "ms")
    assert got["unit"] == "ms" and got["n"] == 1000
    assert got["min"] == 1.0 and got["max"] == 1000.0
    assert got["mean"] == 500.5
    assert got["stdev"] == round(math.sqrt(1000 * 1001 / 12), 3)
    assert got["p50"] == 500.5
    assert got["p90"] == round(1 + 999 * 0.9, 3)
    assert got["p99"] == round(1 + 999 * 0.99, 3)
    assert got["p99_9"] is None            # needs 5000 samples


def test_distribution_has_every_member_in_its_order():
    assert list(distribution([1.0, 2.0], "s")) == ["unit", "n", "min", "p50", "p90", "p99",
                                                   "p99_9", "max", "mean", "stdev"]


def test_a_distribution_is_scaled_and_rounded():
    got = distribution([1500.0, 2500.0], "us", scale=1e-3, decimals=1)
    assert got["min"] == 1.5 and got["max"] == 2.5 and got["mean"] == 2.0
    assert got["stdev"] == round(math.sqrt(0.5), 1)


def test_one_sample_has_no_standard_deviation_and_none_has_no_figures():
    one = distribution([7.0], "us")
    assert one["n"] == 1 and one["min"] == one["max"] == one["mean"] == 7.0
    assert one["stdev"] is None and one["p50"] is None
    empty = distribution([], "us")
    assert empty["n"] == 0 and empty["unit"] == "us"
    assert all(empty[key] is None for key in empty if key not in ("n", "unit"))


def test_a_sample_that_is_not_a_number_is_refused():
    with pytest.raises(ValueError):
        distribution([1.0, float("nan")], "us")
    with pytest.raises(ValueError):
        distribution([float("inf")], "us")


def test_distribution_takes_a_numpy_array():
    assert distribution(np.arange(10.0), "us")["p50"] == 4.5


# ----------------------------------------------------------------------------- raw_samples()


def test_samples_up_to_the_cap_are_kept_in_their_order():
    got = results.raw_samples([3.0, 1.0, 2.0], "us", cap=3)
    assert got == {"unit": "us", "n": 3, "samples": [3.0, 1.0, 2.0]}


def test_more_samples_than_the_cap_become_a_histogram_that_counts_every_one():
    values = list(range(100))
    got = results.raw_samples(values, "us", cap=99, bins=10)
    assert "samples" not in got and got["n"] == 100 and got["cap"] == 99
    assert len(got["histogram"]["edges"]) == 11 and len(got["histogram"]["counts"]) == 10
    assert sum(got["histogram"]["counts"]) == 100
    assert got["histogram"]["edges"][0] == 0.0 and got["histogram"]["edges"][-1] == 99.0
    assert got["histogram"]["counts"] == [10] * 10


def test_the_default_cap_is_the_documented_one():
    assert results.RAW_SAMPLES_CAP == 5000
    assert "samples" in results.raw_samples(range(5000), "us")
    assert "histogram" in results.raw_samples(range(5001), "us")


# --------------------------------------------------------------------- figures and summaries


def test_a_summary_of_figures_distributions_and_groups_passes():
    results.check_summary({"updates": results.figure(50, "updates"),
                           "total": distribution([1.0, 2.0], "s"),
                           "per_step": {"erase": results.figure(1.5, "s")}})


@pytest.mark.parametrize("summary, named", [
    ({"updates": 50}, "summary.updates"),
    ({"group": {"x": 1.0}}, "summary.group.x"),
    ({"x": {"value": 1.0, "unit": ""}}, "summary.x"),
    ({"group": {}}, "summary.group"),
])
def test_a_figure_without_its_unit_is_refused_by_name(summary, named):
    with pytest.raises(ValueError, match=re.escape(named)):
        results.check_summary(summary)


def test_a_figure_is_a_finite_number():
    with pytest.raises(ValueError):
        results.figure(float("nan"), "s")
    assert results.figure(None, "s") == {"value": None, "unit": "s"}


# ------------------------------------------------------------------------------ the build id


#: Who makes the commits of the temporary repositories; a submodule is added from a local path.
_GIT_CONFIG = ("-c", "user.name=t", "-c", "user.email=t@localhost", "-c",
               "protocol.file.allow=always")

needs_cmake_and_git = pytest.mark.skipif(
    shutil.which("cmake") is None or shutil.which("git") is None, reason="needs cmake and git")


def _git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *_GIT_CONFIG, *args], check=True,
                          capture_output=True, text=True).stdout


def _id_from_cmake(repo: Path) -> str:
    """The build id cmake/FcnBuildId.cmake writes for `repo`, as every build of the binaries
    does."""
    out = repo.parent / "fcn_build_id.c"
    subprocess.run(["cmake", f"-DSOURCE_DIR={repo}", f"-DOUTPUT={out}", "-P",
                    str(paths.REPO_ROOT / "cmake" / "FcnBuildId.cmake")],
                   check=True, capture_output=True)
    return re.search(r'fcn_build_id\[\] = "([^"]*)"', out.read_text()).group(1)


def _id_from_python(repo: Path) -> str:
    """tree_build_id() of the provenance provenance() records for `repo`: its commit and the
    files of its status (STATUS_FOR_BUILD_ID, the call provenance() makes)."""
    porcelain = _git(repo, *results.STATUS_FOR_BUILD_ID)
    provenance = {"git_commit": _git(repo, "rev-parse", "HEAD").strip(),
                  "git_modified_files": results._modified_files(porcelain)}
    return results.tree_build_id(provenance, repo=repo)


@needs_cmake_and_git
def test_the_tree_build_id_is_the_one_cmake_writes_into_the_binaries(tmp_path):
    """tree_build_id() and cmake/FcnBuildId.cmake, the two implementations of the rule, agree on
    a clean tree, on two different sets of changes, on changes under results/ only (not dirty)
    and on an untracked file (not part of the id)."""
    repo = tmp_path / "repo"
    repo.mkdir()
    _git(repo, "init", "-q")
    (repo / "a.c").write_text("int a;\n")
    (repo / "results").mkdir()
    (repo / "results" / "r.json").write_text("{}\n")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-qm", "first")
    head = _git(repo, "rev-parse", "HEAD").strip()

    def from_cmake() -> str:
        return _id_from_cmake(repo)

    def from_python() -> str:
        return _id_from_python(repo)

    assert from_cmake() == from_python() == head
    (repo / "a.c").write_text("int a = 1;\n")
    first = from_cmake()
    assert first == from_python() and re.fullmatch(rf"{head}-dirty-[0-9a-f]{{12}}", first)
    (repo / "a.c").write_text("int a = 2;\n")
    second = from_cmake()
    assert second == from_python() and second != first
    _git(repo, "checkout", "-q", "--", "a.c")
    (repo / "results" / "r.json").write_text('{"x": 1}\n')
    (repo / "new.c").write_text("int b;\n")
    assert from_cmake() == from_python() == head
    assert results.BUILD_ID.match(first) and results.BUILD_ID.match(head)


@needs_cmake_and_git
def test_the_build_id_counts_the_submodules(tmp_path):
    """The kernel the node runs on is a submodule (third_party/FreeRTOS-Kernel). A tracked file
    changed in a submodule, or a submodule checked out at another commit than the tree records,
    makes the tree dirty in both implementations, each such tree with an id of its own; an
    untracked file in a submodule is not part of the id, as in the tree itself."""
    kernel = tmp_path / "kernel"
    kernel.mkdir()
    _git(kernel, "init", "-q")
    (kernel / "port.c").write_text("int tick;\n")
    _git(kernel, "add", "-A")
    _git(kernel, "commit", "-qm", "V1")
    (kernel / "port.c").write_text("int tick = 1;\n")
    _git(kernel, "commit", "-qam", "V2")
    repo = tmp_path / "repo"
    repo.mkdir()
    _git(repo, "init", "-q")
    (repo / "a.c").write_text("int a;\n")
    _git(repo, "submodule", "--quiet", "add", str(kernel), "third_party/kernel")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-qm", "first")
    head = _git(repo, "rev-parse", "HEAD").strip()
    sub = repo / "third_party" / "kernel"

    assert _id_from_cmake(repo) == _id_from_python(repo) == head
    (sub / "stray.txt").write_text("not built\n")
    assert _id_from_cmake(repo) == _id_from_python(repo) == head

    (sub / "port.c").write_text("int tick = 2;\n")             # an edit inside the kernel
    edited = _id_from_cmake(repo)
    assert edited == _id_from_python(repo)
    assert re.fullmatch(rf"{head}-dirty-[0-9a-f]{{12}}", edited)
    (sub / "port.c").write_text("int tick = 3;\n")             # another edit, another id
    other = _id_from_cmake(repo)
    assert other == _id_from_python(repo) and other not in (head, edited)

    _git(sub, "checkout", "-q", "--", "port.c")
    assert _id_from_cmake(repo) == _id_from_python(repo) == head
    _git(sub, "checkout", "-q", "HEAD~1")                      # the kernel at another commit
    moved = _id_from_cmake(repo)
    assert moved == _id_from_python(repo)
    assert re.fullmatch(rf"{head}-dirty-[0-9a-f]{{12}}", moved) and moved not in (edited, other)
    _git(sub, "checkout", "-q", "-")
    assert _id_from_cmake(repo) == _id_from_python(repo) == head


def test_a_clean_tree_is_its_commit_and_without_git_it_is_unknown():
    commit = "0123456789abcdef0123456789abcdef01234567"
    assert results.tree_build_id({"git_commit": commit, "git_modified_files": []}) == commit
    assert results.tree_build_id({"git_commit": "unknown", "git_modified_files": []}) == "unknown"


def test_build_id_mismatch_says_what_platform_timing_has_always_said():
    commit = "0123456789abcdef0123456789abcdef01234567"
    clean = {"git_commit": commit, "git_modified_files": []}
    assert results.build_id_mismatch({commit}, clean) is None
    other = "f" * 40
    assert results.build_id_mismatch({other}, clean) == (
        f"the self-test reports build id {other}, but the tree is {commit}: the binary was not "
        "built from the tree the result would name")
    # Two ids from one bench: one of them cannot be the tree's.
    assert results.build_id_mismatch({commit, other}, clean) is not None
    assert "unknown" in results.build_id_mismatch(
        {commit}, {"git_commit": "unknown", "git_modified_files": []})


def _program(tmp_path: Path, name: str, output: str, status: int = 0) -> Path:
    """A stand-in for fcn_node / fcn_boot / fcn_flash: prints `output` and exits `status`."""
    path = tmp_path / name
    path.write_text(f"#!/bin/sh\nprintf '%s\\n' '{output}'\nexit {status}\n")
    path.chmod(0o755)
    return path


@pytest.mark.parametrize("output", [
    "fcn_node 0.1.0 (FreeRTOS V11.3.3) build {id}",
    "fcn_boot 0.1.0 build {id}",
    "fcn_flash 0.1.0 build {id}",
])
def test_the_build_id_of_each_program_is_read_from_its_version_line(tmp_path, output):
    dirty = "0123456789abcdef0123456789abcdef01234567-dirty-0123456789ab"
    program = _program(tmp_path, "p", output.format(id=dirty))
    assert results.binary_build_id(program) == dirty


@pytest.mark.parametrize("output, status", [
    ("fcn_node 0.1.0 (FreeRTOS V11.3.3)", 0),          # a node from before phase 6
    ("fcn_boot 0.1.0 build not-an-id", 0),
    ("fcn_boot 0.1.0 build " + "a" * 40, 1),           # it did not run in order
])
def test_a_program_that_does_not_report_a_build_id_is_refused(tmp_path, output, status):
    with pytest.raises(results.BuildIdMismatch, match="does not report a build id"):
        results.binary_build_id(_program(tmp_path, "p", output, status))


def test_a_program_that_does_not_run_is_refused(tmp_path):
    with pytest.raises(results.BuildIdMismatch, match="cannot be run"):
        results.binary_build_id(tmp_path / "missing")


def test_require_tree_build_names_every_binary_and_report_that_is_not_the_tree():
    commit = "0123456789abcdef0123456789abcdef01234567"
    clean = {"git_commit": commit, "git_modified_files": []}
    assert results.require_tree_build({}, {"node statistics": commit}, clean) == commit
    with pytest.raises(results.BuildIdMismatch) as raised:
        results.require_tree_build({}, {"node statistics": "a" * 40, "run 2": commit}, clean)
    message = str(raised.value)
    assert f"the tree is build {commit}" in message
    assert f"node statistics reports build {'a' * 40}" in message and "run 2" not in message


def test_require_tree_build_asks_each_binary(tmp_path):
    commit = "0123456789abcdef0123456789abcdef01234567"
    clean = {"git_commit": commit, "git_modified_files": []}
    good = _program(tmp_path, "good", f"fcn_node 0.1.0 (FreeRTOS V11.3.3) build {commit}")
    bad = _program(tmp_path, "bad", f"fcn_boot 0.1.0 build {'b' * 40}")
    assert results.require_tree_build({"fcn_node": good}, None, clean) == commit
    with pytest.raises(results.BuildIdMismatch, match=re.escape(f"fcn_boot ({bad}) is build")):
        results.require_tree_build({"fcn_node": good, "fcn_boot": bad}, None, clean)


# ------------------------------------------------------------------------------- the script

RUN_ALL = paths.REPO_ROOT / "bench" / "run_all.py"


def _checkout(root: Path) -> Path:
    """A directory that is a checkout of this project to the walk up from a script: it has a
    CMakePresets.json, and a bench/ directory."""
    (root / "bench").mkdir(parents=True)
    (root / "CMakePresets.json").write_text("{}\n")
    return root


def test_a_bench_of_this_checkout_is_its_own():
    assert results.script_mismatch(RUN_ALL) is None
    assert results.script_mismatch() is None                  # sys.argv[0], the fixture's


def test_the_same_bytes_at_the_same_path_elsewhere_are_this_checkout_s_script(tmp_path):
    # As /home/dev/fcn and /mnt/c/... are one tree under WSL: the bytes decide, not the path.
    copy = _checkout(tmp_path / "mount") / "bench" / "run_all.py"
    copy.write_bytes(RUN_ALL.read_bytes())
    assert results.script_mismatch(copy) is None


def test_a_changed_script_of_another_checkout_is_refused(tmp_path):
    other = _checkout(tmp_path / "other")
    copy = other / "bench" / "run_all.py"
    copy.write_bytes(RUN_ALL.read_bytes() + b"# another checkout's change\n")
    message = results.script_mismatch(copy)
    assert message is not None
    assert f"{copy} is not {RUN_ALL}" in message and f"PYTHONPATH={other / 'tools'}" in message


def test_a_script_this_checkout_does_not_have_is_refused(tmp_path):
    new = _checkout(tmp_path / "other") / "bench" / "not_in_this_checkout.py"
    new.write_text("print('figures')\n")
    assert f"{new} is not {paths.REPO_ROOT / 'bench' / new.name}" in results.script_mismatch(new)


def test_a_script_in_no_checkout_is_refused(tmp_path):
    loose = tmp_path / "run_all.py"
    loose.write_bytes(RUN_ALL.read_bytes())
    assert not any((parent / "CMakePresets.json").exists() for parent in loose.parents)
    assert "lies in no checkout" in results.script_mismatch(loose)


def test_every_check_of_a_bench_refuses_a_script_of_another_checkout(tmp_path, monkeypatch):
    """require_tree_build() before the bench measures, write_result() when it writes (nothing
    is written) and build_id_mismatch() for platform_timing.py, although every binary and
    every reported id is the tree's."""
    copy = _checkout(tmp_path / "other") / "bench" / "platform_timing.py"
    copy.write_text("# another checkout's analysis\n")
    monkeypatch.setattr(sys, "argv", [str(copy), "--seconds", "10"])
    clean = results.provenance()
    tree = results.tree_build_id(clean)
    node = _program(tmp_path, "fcn_node", f"fcn_node 0.1.0 (FreeRTOS V11.3.3) build {tree}")
    refused = re.escape(f"{copy} is not")
    with pytest.raises(results.BuildIdMismatch, match=refused):
        results.require_tree_build({"fcn_node": node}, {"node statistics": tree}, clean)
    with pytest.raises(results.BuildIdMismatch, match=refused):
        results.write_result("probe", {}, out_dir=tmp_path / "out", binaries={"fcn_node": node},
                             reported_build_ids={"node statistics": tree})
    assert not (tmp_path / "out" / "probe.json").exists()
    assert f"{copy} is not" in results.build_id_mismatch({tree}, clean)


# ------------------------------------------------------------------------------- the load


def test_an_idle_host_passes_and_is_not_marked():
    check = results.check_load(limit=1.0, load=0.3)
    assert check == results.LoadCheck(limit=1.0, load_at_start=0.3, loaded=False)
    assert not results.check_load(limit=1.0, load=1.0).loaded     # at the limit: not above it


def test_a_loaded_host_is_refused_unless_allowed_and_then_marked():
    with pytest.raises(results.HostLoaded, match="above the limit 1.0"):
        results.check_load(limit=1.0, load=1.01)
    assert results.check_load(limit=1.0, allow_loaded=True, load=4.0).loaded is True


def test_the_load_at_start_is_the_one_taken_at_import(monkeypatch):
    monkeypatch.setattr(results, "_LOAD_AT_START", 7.5)
    with pytest.raises(results.HostLoaded):
        results.check_load(limit=1.0)
    monkeypatch.setattr(results, "_LOAD_AT_START", None)      # no load average on this system
    assert results.check_load(limit=1.0).loaded is False


@pytest.mark.parametrize("limit", [0.0, -1.0, float("nan")])
def test_a_load_limit_must_be_positive(limit):
    with pytest.raises(ValueError):
        results.check_load(limit=limit, load=0.1)


# ------------------------------------------------------------------------ the command line


def _bench_args(*argv: str) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    results.add_bench_arguments(parser, quick="a few seconds")
    return parser.parse_args(list(argv))


def test_the_options_of_every_bench_and_their_defaults():
    args = _bench_args()
    assert (args.quick, args.out, args.replot, args.allow_loaded, args.no_build) == (
        False, None, False, False, False)
    assert args.max_load == results.DEFAULT_LOAD_LIMIT
    assert results.output_dir(args) == paths.results_dir()


def test_out_names_the_directory(tmp_path):
    assert results.output_dir(_bench_args("--out", str(tmp_path), "--quick")) == tmp_path


def test_a_quick_run_never_writes_into_results(capsys):
    with pytest.raises(SystemExit) as raised:
        results.output_dir(_bench_args("--quick"))
    assert raised.value.code == 2
    assert "--out" in capsys.readouterr().err


# ---------------------------------------------------------------------------- write_result()


def test_without_the_new_arguments_a_result_is_what_platform_timing_has_always_written(tmp_path):
    path = results.write_result("probe", {"tick": {"a": 1}}, out_dir=tmp_path)
    written = json.loads(path.read_text())
    assert path == tmp_path / "probe.json"
    assert list(written) == ["schema", "provenance", "host_timing_note", "tick"]
    assert written["schema"] == "fcn.result/probe/1"
    assert written["host_timing_note"] == results.HOST_TIMING_NOTE
    assert written["tick"] == {"a": 1}
    assert path.read_text().endswith("}\n")
    assert path.stat().st_mode & 0o777 == 0o644
    assert not [p for p in tmp_path.iterdir() if p.name != "probe.json"]   # no temporary left


def test_a_bench_result_has_every_member_in_its_order(tmp_path):
    load = results.LoadCheck(limit=1.0, load_at_start=2.0, loaded=True)
    summary = {"total": distribution([1.0, 2.0], "s")}
    path = results.write_result("probe", {"runs": [1, 2]}, summary, {"trials": 2}, tmp_path,
                                version=3, timing_note="Bus times are the model's.", load=load)
    written = json.loads(path.read_text())
    assert list(written) == ["schema", "provenance", "host_timing_note", "loaded", "load_limit",
                             "config", "summary", "runs"]
    assert written["schema"] == "fcn.result/probe/3"
    assert written["host_timing_note"] == f"{results.HOST_TIMING_NOTE} Bus times are the model's."
    assert written["loaded"] is True and written["load_limit"] == 1.0
    assert written["config"] == {"trials": 2} and written["summary"] == summary


@pytest.mark.parametrize("key", results.RESERVED)
def test_the_data_cannot_take_a_member_of_the_skeleton(tmp_path, key):
    with pytest.raises(ValueError, match=key):
        results.write_result("probe", {key: 1}, out_dir=tmp_path)
    assert not (tmp_path / "probe.json").exists()


def test_a_summary_without_units_writes_nothing(tmp_path):
    with pytest.raises(ValueError):
        results.write_result("probe", {}, {"updates": 50}, out_dir=tmp_path)
    assert not (tmp_path / "probe.json").exists()


def _tree_id() -> str:
    """The tree's build id. Each call runs git status over the submodules as well, seconds on
    the 9p mount of a Windows drive: a test asks once."""
    return results.tree_build_id(results.provenance())


def test_a_result_of_binaries_of_this_tree_records_their_build(tmp_path):
    tree = _tree_id()
    node = _program(tmp_path, "fcn_node", f"fcn_node 0.1.0 (FreeRTOS V11.3.3) build {tree}")
    out = tmp_path / "out"
    path = results.write_result("probe", {}, out_dir=out, binaries={"fcn_node": node},
                                reported_build_ids={"node statistics": tree},
                                binary_sha256={"fcn_node": results.sha256_of(node)})
    build = json.loads(path.read_text())["build"]
    assert build["tree"] == tree
    assert build["binaries"]["fcn_node"] == {"path": str(node), "build_id": tree,
                                             "sha256": results.sha256_of(node)}
    assert build["reported"] == {"node statistics": tree}


def test_a_result_of_another_build_is_not_written_and_the_old_one_stays(tmp_path):
    node = _program(tmp_path, "fcn_node", f"fcn_node 0.1.0 (FreeRTOS V11.3.3) build {'c' * 40}")
    out = tmp_path / "out"
    out.mkdir()
    (out / "probe.json").write_text("old\n")
    with pytest.raises(results.BuildIdMismatch):
        results.write_result("probe", {}, out_dir=out, binaries={"fcn_node": node})
    with pytest.raises(results.BuildIdMismatch):
        results.write_result("probe", {}, out_dir=out, reported_build_ids={"stats": "c" * 40})
    assert (out / "probe.json").read_text() == "old\n"


def test_a_binary_that_changed_while_measured_is_refused(tmp_path):
    node = _program(tmp_path, "fcn_node", f"fcn_node 0.1.0 (FreeRTOS V11.3.3) build {_tree_id()}")
    with pytest.raises(results.BuildIdMismatch, match="changed while"):
        results.write_result("probe", {}, out_dir=tmp_path, binaries={"fcn_node": node},
                             binary_sha256={"fcn_node": "0" * 64})
    assert not (tmp_path / "probe.json").exists()


def test_a_tree_that_changed_during_the_write_puts_the_old_result_back(tmp_path, monkeypatch):
    out = tmp_path / "out"
    out.mkdir()
    (out / "probe.json").write_text("old\n")
    answers = iter([None, "the tree changed"])
    monkeypatch.setattr(results, "binaries_mismatch", lambda *args: next(answers))
    with pytest.raises(results.BuildIdMismatch, match="the tree changed"):
        results.write_result("probe", {}, out_dir=out, reported_build_ids={"stats": "x"})
    assert (out / "probe.json").read_text() == "old\n"
    answers = iter([None, "the tree changed"])
    with pytest.raises(results.BuildIdMismatch):
        results.write_result("fresh", {}, out_dir=out, reported_build_ids={"stats": "x"})
    assert not (out / "fresh.json").exists()
