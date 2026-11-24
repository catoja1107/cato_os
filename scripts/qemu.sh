#!/bin/sh
# Boots a disk image in QEMU with OVMF. The boot image is attached as a read-only
# USB drive, and saved files live on a separate virtual USB drive.
# Usage: qemu.sh <run|debug|headless> <disk.img>
#
# In run and debug modes, saved files are copied from the real Cato USB drive (E:)
# before boot and back after QEMU exits, if that drive is plugged in.
set -eu

mode=${1:-run}
disk=${2:-build/cato.img}
ovmf_code=/usr/share/OVMF/OVMF_CODE_4M.fd
ovmf_vars_template=/usr/share/OVMF/OVMF_VARS_4M.fd
display=${CATO_DISPLAY:-gtk} # Set CATO_DISPLAY=none to run without a window.
store=${CATO_STORE_IMG:-storage/cato-store.img}

for file in "$disk" "$ovmf_code" "$ovmf_vars_template"; do
    if [ ! -f "$file" ]; then
        echo "qemu.sh: missing $file" >&2
        exit 1
    fi
done
if [ ! -f "$store" ]; then
    sh scripts/mkstore.sh "$store"
fi

# QEMU gets a private, freshly copied NVRAM store on every boot.
ovmf_vars=$(mktemp /tmp/cato-ovmf-vars.XXXXXX)
trap 'rm -f "$ovmf_vars"' EXIT INT TERM
cp "$ovmf_vars_template" "$ovmf_vars"

sync_usb=1
case "$mode" in
    run)
        accel=kvm:tcg
        set -- -display "$display" -serial stdio -no-reboot
        ;;
    debug)
        # Interrupt logging requires TCG. A triple fault pauses instead of resetting.
        accel=tcg
        set -- -display "$display" -serial stdio -s -S \
            -d int,cpu_reset,guest_errors -D build/qemu.log \
            -action reboot=shutdown,shutdown=pause
        echo "QEMU is paused. In another terminal: gdb -ex 'target remote :1234'"
        ;;
    headless)
        accel=kvm:tcg
        sync_usb=0 # Automated tests must never touch the real drive.
        set -- -display none -serial "file:${CATO_SERIAL_LOG:-build/serial.log}" \
            -debugcon "file:${CATO_DEBUG_LOG:-build/debugcon.log}" \
            -monitor stdio -no-reboot
        ;;
    *)
        echo "usage: $0 <run|debug|headless> <disk.img>" >&2
        exit 2
        ;;
esac

# Extra arguments for tests, such as additional virtual disks.
# shellcheck disable=SC2086
set -- "$@" ${CATO_QEMU_ARGS:-}

if [ "$sync_usb" -eq 1 ]; then
    python3 scripts/usb_sync.py pull "$store" || echo "qemu.sh: could not read the USB drive" >&2
fi

status=0
qemu-system-x86_64 \
    -machine q35,accel="$accel" \
    -cpu max \
    -smp 2 \
    -m 512M \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$ovmf_code" \
    -drive if=pflash,format=raw,unit=1,file="$ovmf_vars" \
    -device qemu-xhci,id=xhci \
    -drive if=none,id=usbdisk,format=raw,readonly=on,file="$disk" \
    -device usb-storage,bus=xhci.0,drive=usbdisk,bootindex=0 \
    -drive if=none,id=catostore,format=raw,file="$store" \
    -device usb-storage,bus=xhci.0,drive=catostore,removable=on \
    -netdev user,id=net0 \
    -device virtio-net-pci,netdev=net0,romfile= \
    "$@" || status=$?

if [ "$sync_usb" -eq 1 ]; then
    python3 scripts/usb_sync.py push "$store" || echo "qemu.sh: could not save to the USB drive" >&2
fi
exit "$status"
