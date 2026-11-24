#!/bin/sh
# Installs the Cato OS build and test toolchain on Ubuntu 24.04 (native or WSL).
# Only installs packages. It does not touch disks, partitions, or firmware.
set -eu

# python3-pil is used only by the tests, as an independent reference image decoder.
packages="clang libclang-rt-dev lld make mtools gdb gdisk python3 python3-pil openssl qemu-system-x86 qemu-system-gui ovmf"

if [ "$(id -u)" -eq 0 ]; then
    sudo_cmd=""
else
    sudo_cmd="sudo"
fi

$sudo_cmd apt-get update
# shellcheck disable=SC2086  # the package list is intentionally word-split
$sudo_cmd env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $packages

echo
echo "Toolchain ready:"
clang --version | head -n 1
lld-link --version
qemu-system-x86_64 --version | head -n 1
ls /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_VARS_4M.fd
