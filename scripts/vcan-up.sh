#!/usr/bin/env bash
# Bring up a virtual SocketCAN interface and, with --verify, prove that it carries a frame.
#   scripts/vcan-up.sh [--verify] [--install-modules-extra] [interface]     (default: vcan0)
#
# If the interface already exists and is up, nothing is changed and no privilege is needed.
# Otherwise the script needs root for the kernel module and the interface. Unless it already
# is root it uses sudo: without a prompt where that works (CI, a sudo session that is still
# open), with sudo's own password prompt on a terminal.
#
# Where the vcan module comes from, in this order:
#   1. the running kernel (modprobe vcan): any normal Linux, and GitHub's runners
#   2. with --install-modules-extra: the distribution's linux-modules-extra package for the
#      running kernel, then modprobe again (older runner images kept vcan there)
#   3. a module file built for the running kernel, named by FCN_VCAN_KO or found at
#      ~/.local/share/fcn/vcan-<kernel release>.ko: the way to get vcan on the stock WSL2
#      kernel, which is built without it (docs/wsl-vcan.md). When the script runs under sudo,
#      the home directory of the user who called sudo is searched as well.
set -euo pipefail

verify=0
install_extra=0
iface="vcan0"
for arg in "$@"; do
    case "$arg" in
        --verify) verify=1 ;;
        --install-modules-extra) install_extra=1 ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *) iface="$arg" ;;
    esac
done

# Privilege is settled only when something needs it: sudo_cmd is filled in by the first call.
sudo_ready=0
sudo_cmd=()
as_root() {
    if [[ "$sudo_ready" -eq 0 ]]; then
        if [[ "$(id -u)" -ne 0 ]]; then
            if sudo -n true 2>/dev/null; then
                sudo_cmd=(sudo -n)
            elif [[ -t 0 ]]; then
                sudo_cmd=(sudo) # asks for the password once; later calls reuse the session
                sudo -v
            else
                echo "error: this needs root, sudo asks for a password and there is no terminal" >&2
                echo "       to ask on. Run 'sudo -v' first, or run this script from a terminal." >&2
                exit 1
            fi
        fi
        sudo_ready=1
    fi
    "${sudo_cmd[@]}" "$@"
}

# The module file built by scripts/wsl/build-vcan-module.sh, wherever it may be.
module_file() {
    local release candidates=() home
    release="$(uname -r)"
    [[ -n "${FCN_VCAN_KO:-}" ]] && candidates+=("$FCN_VCAN_KO")
    candidates+=("$HOME/.local/share/fcn/vcan-$release.ko")
    if [[ -n "${SUDO_USER:-}" ]]; then
        home="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
        [[ -n "$home" ]] && candidates+=("$home/.local/share/fcn/vcan-$release.ko")
    fi
    local file
    for file in "${candidates[@]}"; do
        if [[ -f "$file" ]]; then
            echo "$file"
            return 0
        fi
    done
    return 1
}

load_vcan() {
    as_root true # settle the privilege first, where its messages are not discarded
    as_root modprobe vcan 2>/dev/null && return 0

    if [[ "$install_extra" -eq 1 ]]; then
        echo "vcan is not in the running kernel's modules; installing linux-modules-extra"
        as_root apt-get update -qq
        as_root apt-get install -y -qq "linux-modules-extra-$(uname -r)" &&
            as_root modprobe vcan && return 0
    fi

    local ko
    if ko="$(module_file)"; then
        echo "loading $ko"
        as_root modprobe can
        as_root modprobe can_raw
        as_root modprobe can_dev
        as_root insmod "$ko" && return 0
    fi

    return 1
}

is_up() {
    ip link show "$iface" 2>/dev/null | head -n 1 | grep -q '<[^>]*\bUP\b'
}

if ! ip link show "$iface" >/dev/null 2>&1; then
    if ! grep -q '^vcan ' /proc/modules 2>/dev/null; then
        if ! load_vcan; then
            echo "error: no vcan module for kernel $(uname -r)." >&2
            echo "       On WSL2 see docs/wsl-vcan.md." >&2
            exit 1
        fi
    fi
    as_root ip link add dev "$iface" type vcan
fi
is_up || as_root ip link set up "$iface"
ip -brief link show "$iface"

if [[ "$verify" -eq 1 ]]; then
    # One frame sent with cansend has to arrive in candump.
    log="$(mktemp)"
    timeout 5 candump -n 1 "$iface,123:7FF" >"$log" 2>&1 &
    dump_pid=$!
    sleep 0.5
    cansend "$iface" 123#DEADBEEF
    if wait "$dump_pid" && grep -q ' 123 ' "$log"; then
        echo "verified: $(tr -s ' ' <"$log")"
        rm -f "$log"
    else
        echo "error: the frame sent on $iface did not come back" >&2
        cat "$log" >&2
        rm -f "$log"
        exit 1
    fi
fi
