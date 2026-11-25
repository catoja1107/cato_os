#!/bin/sh
# Builds a GPT disk image FILE containing a FAT32 EFI System Partition.
# Usage: mkimage.sh <BOOTX64.EFI> <output.img>
#
# Safety: this script only ever writes a regular file ending in .img. It refuses
# device paths, and it refuses to replace an existing file outside build/.
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <BOOTX64.EFI> <output.img>" >&2
    exit 2
fi

efi=$1
repo=$(cd "$(dirname "$0")/.." && pwd -P)
out=$(readlink -m -- "$2")

refuse() {
    echo "mkimage: refusing '$out': $1" >&2
    exit 1
}

# Firmware silently skips a loader without a PE header and reports "Not Found".
# A zero-filled file from an interrupted write would otherwise pass every check below.
if [ "$(head -c 2 -- "$efi")" != MZ ]; then
    echo "mkimage: '$efi' is not an EFI executable (no MZ header). Run make again to rebuild it." >&2
    exit 1
fi

case "$out" in
    /dev/*|/sys/*|/proc/*) refuse "this is a device or kernel path" ;;
    *.img) ;;
    *) refuse "the output name must end in .img" ;;
esac

if [ -e "$out" ]; then
    [ -f "$out" ] || refuse "it exists and is not a regular file"
    case "$out" in
        "$repo"/build/*) ;;
        *) refuse "it already exists outside build/" ;;
    esac
fi

# 64 MiB disk: 1 MiB for the GPT, a 62 MiB ESP, and 1 MiB for the backup GPT.
# With 512-byte clusters the ESP has more than the 65,525 clusters FAT32 requires.
sector_size=512
disk_sectors=131072
esp_start=2048
esp_end=$((disk_sectors - 2048 - 1))
esp_sectors=$((esp_end - esp_start + 1))
esp_offset=$((esp_start * sector_size))

mkdir -p "$(dirname "$out")"
tmp="$out.tmp.$$"
trap 'rm -f "$tmp"' EXIT INT TERM

truncate -s $((disk_sectors * sector_size)) "$tmp"
sgdisk --clear \
    --new=1:"$esp_start":"$esp_end" \
    --typecode=1:ef00 \
    --change-name=1:"EFI System Partition" \
    "$tmp" >/dev/null

export MTOOLS_SKIP_CHECK=1
esp="$tmp@@$esp_offset"
mformat -i "$esp" -F -T "$esp_sectors" -H "$esp_start" -c 1 -v CATO_ESP ::
mmd -i "$esp" ::/EFI ::/EFI/BOOT
mcopy -i "$esp" "$efi" ::/EFI/BOOT/BOOTX64.EFI

# Read the loader back before installing the image. Otherwise an image whose
# writes were lost would get a fresh timestamp, and make would never rebuild it.
if ! mcopy -n -i "$esp" ::/EFI/BOOT/BOOTX64.EFI - | cmp -s - "$efi"; then
    echo "mkimage: verification failed: EFI/BOOT/BOOTX64.EFI in the image does not match $efi" >&2
    exit 1
fi

mv -f "$tmp" "$out"
trap - EXIT INT TERM
echo "Image ready: $out"
