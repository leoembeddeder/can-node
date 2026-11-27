"""Where the repository, its build trees and its binaries are.

The build root follows the same rule as the shell scripts: FCN_BUILD_ROOT if set, otherwise
<repo>/build. A binary can be overridden explicitly with FCN_NODE / FCN_SELFTEST.
"""
from __future__ import annotations

import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def build_root() -> Path:
    return Path(os.environ.get("FCN_BUILD_ROOT", REPO_ROOT / "build"))


def node_binary(preset: str = "release") -> Path:
    override = os.environ.get("FCN_NODE")
    return Path(override) if override else build_root() / preset / "bin" / "fcn_node"


def boot_binary(preset: str = "release") -> Path:
    """fcn_boot, the bootloader of the host build (platform/posix/boot_main.c); FCN_BOOT
    overrides it, as FCN_NODE does the node."""
    override = os.environ.get("FCN_BOOT")
    return Path(override) if override else build_root() / preset / "bin" / "fcn_boot"


def flash_tool_binary(preset: str = "release") -> Path:
    """fcn_flash, the flash tool (platform/posix/flash_tool.c); FCN_FLASH_TOOL overrides it."""
    override = os.environ.get("FCN_FLASH_TOOL")
    return Path(override) if override else build_root() / preset / "bin" / "fcn_flash"


def tool_binary(name: str, preset: str = "release") -> Path:
    """A program of tests/tools/, for example "can_bits_cli": the build puts them next to the
    node. They exist only to be run from a build tree by the tests, so there is no override."""
    return build_root() / preset / "bin" / name


def platform_test_binary(name: str, preset: str = "release") -> Path:
    """A self-test program of tests/platform other than plat_selftest, for example
    "wdg_selftest". Like tool_binary(), only ever run from a build tree, so no override."""
    return build_root() / preset / "tests" / "platform" / name


def selftest_binary(preset: str = "release") -> Path:
    override = os.environ.get("FCN_SELFTEST")
    if override:
        return Path(override)
    return build_root() / preset / "tests" / "platform" / "plat_selftest"


def results_dir() -> Path:
    return REPO_ROOT / "results"
