"""bench/run_all.py, the final runs: the plan, the wait for an idle machine and the stop at the
first failure, with constructed loads and constructed bench runs."""
from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import pytest

from fcn_tools import paths, results


def _load_run_all():
    path = paths.REPO_ROOT / "bench" / "run_all.py"
    spec = importlib.util.spec_from_file_location("bench_run_all", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module      # dataclasses look their module up by name
    spec.loader.exec_module(module)
    return module


run_all = _load_run_all()


def test_the_plan_ends_with_platform_timing_and_every_script_exists():
    names = [bench.name for bench in run_all.PLAN]
    assert names[-1] == "platform_timing"
    assert len(set(names)) == len(names)
    for bench in run_all.PLAN:
        script = paths.REPO_ROOT / bench.argv[0]
        assert script.is_file(), script
        assert script.name == f"{bench.name}.py"


def test_the_plan_is_in_the_order_bench_readme_gives():
    readme = (paths.REPO_ROOT / "bench" / "README.md").read_text()
    order = readme.split("## The final runs", 1)[1]
    positions = [order.find(f". {bench.name}") for bench in run_all.PLAN]
    assert all(position >= 0 for position in positions), positions
    assert positions == sorted(positions)


class _Clock:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


def test_the_wait_ends_when_the_load_has_come_down():
    clock = _Clock()
    loads = iter([3.0, 2.0, 1.2, 0.9, 5.0])
    said = []
    waited = run_all.wait_for_idle(1.0, 100.0, poll=10.0, load=lambda: next(loads),
                                   sleep=clock.sleep, clock=clock, say=said.append)
    assert waited == 30.0
    assert len(said) == 1 and "3.00" in said[0]


def test_an_idle_machine_is_not_waited_for():
    clock = _Clock()
    assert run_all.wait_for_idle(1.0, 100.0, load=lambda: 1.0, sleep=clock.sleep,
                                 clock=clock, say=lambda _: None) == 0.0


def test_the_wait_gives_up_after_its_timeout():
    clock = _Clock()
    with pytest.raises(TimeoutError, match="still 2.00"):
        run_all.wait_for_idle(1.0, 30.0, poll=10.0, load=lambda: 2.0, sleep=clock.sleep,
                              clock=clock, say=lambda _: None)
    assert clock.now == 30.0


PLAN = (run_all.Bench("a", ("bench/a.py",)), run_all.Bench("b", ("bench/b.py",)),
        run_all.Bench("c", ("bench/c.py",)))


def test_the_plan_stops_at_the_first_failure_with_its_status():
    ran = []
    statuses = {"a": 0, "b": 4, "c": 0}
    said = []

    def run(bench) -> int:
        ran.append(bench.name)
        return statuses[bench.name]

    assert run_all.run_plan(PLAN, wait=lambda: 0.0, run=run, say=said.append) == 4
    assert ran == ["a", "b"]
    assert any("not run: c" in line for line in said)


def test_every_bench_waits_for_an_idle_machine_first():
    events = []
    run_all.run_plan(PLAN, wait=lambda: events.append("wait") or 0.0,
                     run=lambda bench: events.append(bench.name) or 0, say=lambda _: None)
    assert events == ["wait", "a", "wait", "b", "wait", "c"]


def test_a_part_of_the_plan_is_chosen_by_name():
    assert [b.name for b in run_all.select(PLAN, start="b")] == ["b", "c"]
    assert [b.name for b in run_all.select(PLAN, only="b")] == ["b"]
    assert run_all.select(PLAN) == list(PLAN)
    with pytest.raises(ValueError, match="no bench d"):
        run_all.select(PLAN, start="d")


def test_a_dirty_tree_is_refused(monkeypatch, capsys):
    monkeypatch.setattr(run_all, "dirty_files", lambda: ["app/app.c"])
    built = []
    monkeypatch.setattr(run_all.subprocess, "run", lambda *a, **k: built.append(a))
    assert run_all.main([]) == 1
    assert "app/app.c" in capsys.readouterr().err
    assert built == []


def test_list_prints_the_plan_and_runs_nothing(capsys):
    assert run_all.main(["--list"]) == 0
    assert "platform_timing" in capsys.readouterr().out


def _run_copy(script: Path, *args: str) -> subprocess.CompletedProcess:
    """`script` (a copy of run_all.py) run as the final runs run it, with fcn_tools of this
    checkout, whatever the virtual environment's editable install points at."""
    env = dict(os.environ, PYTHONPATH=str(paths.REPO_ROOT / "tools"))
    return subprocess.run([sys.executable, str(script), *args], capture_output=True, text=True,
                          timeout=60, cwd=script.parent, env=env, check=False)


def _copy(tmp_path: Path, where: str, change: bytes = b"") -> Path:
    """run_all.py copied into another checkout (a directory with a CMakePresets.json) or into no
    checkout, with `change` appended."""
    source = (paths.REPO_ROOT / "bench" / "run_all.py").read_bytes() + change
    if where == "another checkout":
        directory = tmp_path / "other" / "bench"
        directory.mkdir(parents=True)
        (tmp_path / "other" / "CMakePresets.json").write_text("{}\n")
    else:
        directory = tmp_path
    copy = directory / "run_all.py"
    copy.write_bytes(source)
    return copy


@pytest.mark.parametrize("where, change", [
    ("another checkout", b"# another checkout's change\n"),
    ("no checkout", b""),
], ids=["another checkout", "no checkout"])
def test_a_run_all_that_is_not_this_checkout_s_runs_nothing(tmp_path, where, change):
    # --list, which starts no bench even if the refusal were missing.
    done = _run_copy(_copy(tmp_path, where, change), "--list")
    assert done.returncode == results.EXIT_BUILD_MISMATCH, done.stdout + done.stderr
    assert done.stdout == "" and done.stderr.startswith("no bench run: the bench script ")


def test_a_run_all_with_this_checkout_s_bytes_elsewhere_is_its_own(tmp_path):
    # The same tree through another mount (/mnt/c/... for /home/dev/fcn) is not refused.
    done = _run_copy(_copy(tmp_path, "another checkout"), "--list")
    assert done.returncode == 0, done.stderr
    assert done.stdout.splitlines()[-1].startswith("platform_timing")
