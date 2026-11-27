#!/usr/bin/env bash
# The code generated from the DBC is what the DBC gives.
#   scripts/codegen-check.sh            # change nothing; exit 1 with a diff if a file is stale
#   scripts/codegen-check.sh --update   # regenerate the files after a change of the DBC
#
# tools/gen_can_db.py generates, from dbc/fcn.dbc,
#   app/gen/can_db.h, app/gen/can_db.c   the codec of the node's CAN messages
#   tests/tools/can_db_table.c           the table of that codec that the tests use
# The files are checked in, so that a build needs neither Python nor cantools; this check, run
# by CI and by scripts/check.sh, is what keeps them from falling behind the DBC. It also fails
# when the DBC breaks one of the generator's rules (tools/gen_can_db.py lists them).
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

python="$(fcn_python)"
if ! "$python" -c 'import cantools' 2>/dev/null; then
    echo "cantools missing in $FCN_VENV (run scripts/setup-python.sh)" >&2
    exit 1
fi

cd "$FCN_ROOT"
generate=("$python" tools/gen_can_db.py dbc/fcn.dbc app/gen --test-table tests/tools/can_db_table.c)

case "${1:-}" in
"")
    if "${generate[@]}" --check; then
        echo "codegen: app/gen/can_db.[ch] and tests/tools/can_db_table.c are what dbc/fcn.dbc gives"
    else
        echo "codegen: out of date or the DBC is broken; after fixing the DBC run" \
            "scripts/codegen-check.sh --update" >&2
        exit 1
    fi
    ;;
--update)
    "${generate[@]}"
    echo "codegen: generated app/gen/can_db.[ch] and tests/tools/can_db_table.c from dbc/fcn.dbc"
    ;;
*)
    echo "usage: scripts/codegen-check.sh [--update]" >&2
    exit 2
    ;;
esac
