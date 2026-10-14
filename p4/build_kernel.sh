#!/bin/bash
# Builds and installs Linux 6.8 with fork_guard on Ubuntu 24.04.
# Steps follow Q4_Help_Doc.pdf. Run as a normal user with sudo rights.
#
#   WARNING: take a VM snapshot first and keep the stock kernel in GRUB.
#   Use 4 cores and 6 GB RAM or more; the build needs about 30 GB disk.
#
# Usage: ./build_kernel.sh [work_dir]     (default: $HOME/kbuild)

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-$HOME/kbuild}"

echo "!!! Take a VM snapshot before continuing. Ctrl-C now to abort. !!!"
sleep 8

sudo apt install -y build-essential libncurses-dev bison flex libssl-dev \
                    libelf-dev dwarves zstd bc forkstat python3

mkdir -p "$WORK"
cd "$WORK"
[ -f linux-6.8.tar.xz ] || wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.8.tar.xz
[ -d linux-6.8 ] || tar xf linux-6.8.tar.xz
cd linux-6.8

# our files + hooks into fork.c, sched/core.c and kernel/Makefile
python3 "$HERE/hooks/apply_hooks.py" "$WORK/linux-6.8"

# finish the config before the first build (later changes cause full rebuilds)
cp /boot/config-"$(uname -r)" .config
scripts/config --disable SYSTEM_TRUSTED_KEYS
scripts/config --disable SYSTEM_REVOCATION_KEYS
scripts/config --disable DEBUG_INFO
scripts/config --disable DEBUG_INFO_BTF
scripts/config --set-str LOCALVERSION "-forkguard"
scripts/config --enable SATA_AHCI      # disk driver must be built in (=y)
scripts/config --enable VIRTIO_BLK     # otherwise boot drops to (initramfs)
make olddefconfig

make -j"$(nproc)"
sudo make modules_install install
sudo update-grub

# user-space tool
gcc -Wall -Wextra -O2 -o "$WORK/tier" "$HERE/tools/tier.c"
sudo install -m 755 "$WORK/tier" /usr/local/bin/tier

echo
echo "Done. Reboot, then check:  uname -r   (must end in -forkguard)"
echo "If boot drops to (initramfs): type reboot and pick the old kernel in GRUB > Advanced options."
