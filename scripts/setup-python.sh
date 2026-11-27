#!/usr/bin/env bash
# Create the Python virtual environment and install the pinned tools plus this repository's
# own package (tools/fcn_tools) in editable mode.
#   scripts/setup-python.sh
# The environment is created at $FCN_VENV (default: <repo>/.venv).
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

if [[ ! -x "$FCN_VENV/bin/python" ]]; then
    python3 -m venv "$FCN_VENV"
fi

"$FCN_VENV/bin/python" -m pip install --quiet --upgrade pip
"$FCN_VENV/bin/python" -m pip install --quiet -r "$FCN_ROOT/requirements.txt"
"$FCN_VENV/bin/python" -m pip install --quiet --editable "$FCN_ROOT"
"$FCN_VENV/bin/python" -m pip list --format=freeze |
    grep -i -E '^(python-can|cantools|can-isotp|udsoncan|pytest|pytest-timeout|numpy|matplotlib|gcovr)=='
echo "python environment ready: $FCN_VENV"
