#!/usr/bin/env bash
# Build the vcan kernel module for the running WSL2 kernel.
#   scripts/wsl/build-vcan-module.sh
#
# Why: Microsoft's stock WSL2 kernel ships the CAN core, can-raw and can-dev as loadable
# modules but is built with CONFIG_CAN_VCAN unset, so `sudo modprobe vcan` fails. The module's
# source is in Microsoft's public kernel tree, so only drivers/net/can is built, against the
# exact source tag and the configuration of the running kernel. Nothing of the kernel itself
# is replaced.
#
# Cost: a kernel source download and a partial kernel build; docs/wsl-vcan.md has the figures
# measured on the development laptop, and the script prints its own at the end.
# Result: ~/.local/share/fcn/vcan-<kernel release>.ko, which scripts/vcan-up.sh loads.
#
# The module has to be loaded again after every `wsl --shutdown` or Windows restart
# (scripts/vcan-up.sh does that), and rebuilt after a WSL kernel update: the file name
# carries the kernel release, so a stale module is never loaded into a newer kernel.
#
# About the warning "no symbol version for module_layout" that the kernel logs on loading:
# the kernel is built with CONFIG_MODVERSIONS, but Microsoft does not publish its
# Module.symvers, so the module carries no version records for kernel symbols. The loader
# accepts that with one warning. Compatibility comes from building against the exact source
# tag and .config of the running kernel; the vermagic string is still checked.
set -euo pipefail

release="$(uname -r)"
case "$release" in
    *-microsoft-standard-WSL2*) ;;
    *)
        echo "This is not a stock WSL2 kernel ($release). Try: sudo modprobe vcan" >&2
        exit 1
        ;;
esac

base="${release%%-microsoft-standard-WSL2*}"
tag="linux-msft-wsl-${base}"
work="${FCN_VCAN_BUILD_DIR:-$HOME/.cache/fcn/wsl-kernel}"
out="${FCN_VCAN_KO:-$HOME/.local/share/fcn/vcan-${release}.ko}"
jobs="$(nproc)"

echo "==> kernel $release, source tag $tag"
sudo apt-get update -qq
sudo apt-get install -y -qq --no-install-recommends \
    build-essential bc bison flex libssl-dev libelf-dev dwarves cpio kmod \
    wget ca-certificates xz-utils python3

mkdir -p "$work"
cd "$work"
if [[ ! -d "src-$base" ]]; then
    echo "==> downloading the kernel source"
    wget -q -O src.tar.gz \
        "https://github.com/microsoft/WSL2-Linux-Kernel/archive/refs/tags/${tag}.tar.gz"
    # Unpacked under another name and renamed when complete: an extraction that was
    # interrupted must not be mistaken for the source tree by the next run.
    rm -rf "src-$base.partial"
    mkdir "src-$base.partial"
    tar xzf src.tar.gz -C "src-$base.partial" --strip-components=1
    mv "src-$base.partial" "src-$base"
    rm src.tar.gz
fi
cd "src-$base"

echo "==> configuring from the running kernel's /proc/config.gz, plus CONFIG_CAN_VCAN=m"
zcat /proc/config.gz >.config
scripts/config --module CONFIG_CAN_VCAN
make -s olddefconfig
grep -E '^CONFIG_CAN(_DEV|_VCAN)=' .config

echo "==> preparing the tree for module builds (-j$jobs)"
make -s -j"$jobs" modules_prepare

echo "==> building drivers/net/can"
# Without Module.symvers modpost cannot find symbol versions; report that as a warning.
make -s -j"$jobs" KBUILD_MODPOST_WARN=1 M=drivers/net/can modules

mkdir -p "$(dirname "$out")"
cp drivers/net/can/vcan.ko "$out"
echo "==> built $out"
modinfo "$out" | grep -E '^(vermagic|depends):'
echo "==> took ${SECONDS} s; the kernel source tree in $work uses $(du -sh "$work" | cut -f1)"
echo "Load it and create vcan0 with: scripts/vcan-up.sh --verify"
