# SPDX-License-Identifier: MIT
"""Fixtures of the system tests on top of tests/conftest.py: the images the update tests write
into a flash (tests/system/update_bench.py says how they use them)."""
from __future__ import annotations

from pathlib import Path

import pytest

from fcn_tools import bench
from fcn_tools import paths
from fcn_tools.bench import DATA_SEED  # noqa: F401 - the seed of the "data" images, see below


@pytest.fixture(scope="session")
def images(tmp_path_factory) -> dict[str, Path]:
    """Image files (fcn_tools.image pack, target host unless said): "node-1.0.0", "node-1.1.0"
    and "node-1.4.0", the release build of the node, stripped, under three versions; "crash"
    (1.2.0) and "watchdog" (1.3.0), the test payloads of tests/tools/payload.c, which stand in
    for an image that crashes at once (status 13) and one that hangs until the watchdog resets
    it (status 11); and the images of phase 5's downloads: "crash-stripped" (1.2.0),
    fcn_payload_crash stripped, the smallest image that runs; "data" (1.5.0),
    diag_bench.DATA_PAYLOAD_SIZE bytes drawn from a generator seeded with DATA_SEED, an image
    that validates but is no program, for downloads the bootloader never starts;
    "data-damaged", the same with one bit of its payload flipped after the header was made, as
    a bit that changed in transit; and "data-mps2", the same payload packed for the other target
    (mps2-an385). fcn_tools.bench.make_images() packs them. The asan build of the node does not
    fit a slot (docs/update.md), so the asan run of scripts/test-system.sh starts these images
    as well; its fcn_boot and fcn_flash are the asan ones."""
    release = paths.build_root() / "release" / "bin"
    node = release / "fcn_node"
    payloads = {"crash": release / "fcn_payload_crash", "watchdog": release / "fcn_payload_watchdog"}
    for binary in (node, *payloads.values()):
        if not binary.exists():
            pytest.fail(f"{binary} not found: build it with scripts/build.sh release",
                        pytrace=False)
    return bench.make_images(tmp_path_factory.mktemp("images"), release)
