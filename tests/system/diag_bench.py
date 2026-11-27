# SPDX-License-Identifier: MIT
"""The bench of phase 5's system tests of the diagnostic connection (test_diag_isotp.py,
test_diag_services.py, test_download_faults.py, test_download_cuts.py): the tester's two ends of
the node's connection (RawLink, Uds) and the download as a sequence of plain requests
(download()).

It lives in fcn_tools.tester since phase 6, so that the benches of bench/ use the same tester
outside pytest; this module keeps the name the tests import, with every public name of
fcn_tools.tester (its module text says what each is and the rules of its waits). Nothing here
depends on pytest, so the names are those objects themselves.
"""
from __future__ import annotations

from fcn_tools.tester import *  # noqa: F401,F403 - the tests' names, see the module's text
from fcn_tools.tester import __all__  # noqa: F401
