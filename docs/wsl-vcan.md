# SocketCAN on WSL2: enabling `vcan` (optional)

Nothing in this project needs this step. The node has two CAN transports
([can-simulation.md](can-simulation.md)): one over UDP on the loopback interface, which needs
no kernel support and is the default, and one on SocketCAN. Every test that involves the bus
runs on both; on a machine without `vcan0` the SocketCAN half is skipped, and CI, where the
interface exists, refuses to skip it. This page is for running the SocketCAN half on a
Windows laptop as well.

## The check

On the stock WSL2 kernel:

```bash
sudo modprobe vcan; echo "exit status $?"
```

fails with status 1. The reason is in the kernel's configuration:

```bash
zcat /proc/config.gz | grep -E 'CONFIG_CAN(_RAW|_DEV|_VCAN)?[= ]'
```

On kernel `6.6.87.2-microsoft-standard-WSL2` this prints `CONFIG_CAN=m`, `CONFIG_CAN_RAW=m`,
`CONFIG_CAN_DEV=m` and `# CONFIG_CAN_VCAN is not set`: Microsoft ships the CAN core and the
raw protocol as loadable modules but does not build the virtual interface driver.

## The lightest fix: build only that one module

Replacing the WSL kernel with a custom build works, but it is a long build, a machine-wide
change, and a kernel that no longer follows Microsoft's updates. The module alone is enough:
its source is in Microsoft's public kernel tree, and a module built from the exact source tag
with the running kernel's own configuration loads into the stock kernel.

```bash
scripts/wsl/build-vcan-module.sh
```

The script installs the kernel build prerequisites with apt, downloads the source of the
running kernel release from `github.com/microsoft/WSL2-Linux-Kernel`, enables
`CONFIG_CAN_VCAN=m` on top of `/proc/config.gz`, and builds `drivers/net/can` only. On the
8-core development laptop a run with the source already downloaded took 54 seconds and left a
kernel source tree of 1.5 GB in `~/.cache/fcn/wsl-kernel` (it can be deleted afterwards); the
download is the kernel's source archive, a few hundred megabytes. The script prints its own
time and size when it ends. The result is `~/.local/share/fcn/vcan-<kernel release>.ko`.

Then, and again whenever the WSL virtual machine has restarted:

```bash
scripts/vcan-up.sh --verify
```

loads the CAN core modules and the built module, creates `vcan0` and proves it with a frame
sent by `cansend` and received by `candump`. It needs root and uses `sudo`; where `sudo` asks
for a password it does so on the terminal.

## What to expect

- **Not persistent.** A loaded module lives in the WSL virtual machine's kernel and is gone
  when that machine restarts: after `wsl --shutdown`, after a Windows restart, and also about
  a minute after the last WSL process has exited, because WSL stops an idle machine. Run
  `scripts/vcan-up.sh` at the start of a session; it does nothing if `vcan0` is already up.
- **Tied to the kernel release.** After a WSL kernel update the module has to be built again;
  the file name carries the release, so an old module is never loaded into a new kernel.
- **One warning in the kernel log.** `vcan: no symbol version for module_layout`, followed
  by a note that an out-of-tree module taints the kernel. The stock kernel is built with
  symbol versioning, but Microsoft does not publish the version table a module build would
  need, so the module carries none for kernel symbols and the loader accepts it with that
  warning. The comment at the top of the build script explains why that is sound here.
- **Shared by all WSL distributions.** WSL2 runs every distribution on one kernel, so the
  module is visible from all of them, Docker Desktop's included.
- **One test at a time on `vcan0`.** Every test run on the machine shares the interface, and
  a test that counts frames would count another test's frames. The tests therefore hold an
  exclusive `flock()` on `fcn-test-vcan0.lock` in the temporary directory while they use the
  interface. That directory is the one Python's `tempfile.gettempdir()` picks: the first of
  `TMPDIR`, `TEMP` and `TMP` that names a directory, else `/tmp`. `tests/conftest.py` holds
  the lock around every SocketCAN system test, and `tests/platform/test_can_backend.c` around
  its SocketCAN tests. Anything else that sends on `vcan0` while tests run should take the
  same lock, for example `flock /tmp/fcn-test-vcan0.lock cansend vcan0 123#00`.
