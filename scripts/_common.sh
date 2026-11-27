# Sourced by the other scripts; not meant to be run.
#
# Environment:
#   FCN_BUILD_ROOT  directory with one build tree per CMake preset   (default: <repo>/build)
#   FCN_VENV        Python virtual environment                       (default: <repo>/.venv)
#
# On Windows with WSL, point both at the Linux filesystem (for example ~/fcn-build and
# ~/.venvs/fcn): building on the Windows drive is slow and, under OneDrive, noisy.
set -euo pipefail

FCN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FCN_BUILD_ROOT="${FCN_BUILD_ROOT:-$FCN_ROOT/build}"
FCN_VENV="${FCN_VENV:-$FCN_ROOT/.venv}"
export FCN_ROOT FCN_BUILD_ROOT FCN_VENV

# Keep Python bytecode out of the source tree.
export PYTHONPYCACHEPREFIX="${PYTHONPYCACHEPREFIX:-$FCN_BUILD_ROOT/pycache}"

# Directories that hold project C code (third_party/ is never analysed or reformatted, and
# scripts/selftest/ holds code that is wrong on purpose).
FCN_CODE_DIRS=(lib app platform tests config)

fcn_python() {
    if [[ -x "$FCN_VENV/bin/python" ]]; then
        echo "$FCN_VENV/bin/python"
    else
        echo "python3"
    fi
}

# Configure (if needed) and build one preset into $FCN_BUILD_ROOT/<preset>.
fcn_build() {
    local preset="$1"
    local dir="$FCN_BUILD_ROOT/$preset"
    cmake --preset "$preset" -S "$FCN_ROOT" -B "$dir" >"$dir.configure.log" 2>&1 || {
        cat "$dir.configure.log" >&2
        return 1
    }
    cmake --build "$dir"
}

fcn_project_sources() {
    local dirs=()
    local d
    for d in "${FCN_CODE_DIRS[@]}"; do
        [[ -d "$FCN_ROOT/$d" ]] && dirs+=("$FCN_ROOT/$d")
    done
    find "${dirs[@]}" -type f \( -name '*.c' -o -name '*.h' \) | sort
}
