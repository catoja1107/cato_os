#!/bin/sh
# Creates QEMU's virtual Cato USB drive: a FAT32 image holding the storage marker.
# Usage: mkstore.sh <output.img>
#
# Safety: this only creates a NEW regular file ending in .img.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <output.img>" >&2
    exit 2
fi

out=$(readlink -m -- "$1")
case "$out" in
    /dev/*|/sys/*|/proc/*) echo "mkstore: refusing device path '$out'" >&2; exit 1 ;;
    *.img) ;;
    *) echo "mkstore: output must end in .img" >&2; exit 1 ;;
esac
if [ -e "$out" ]; then
    echo "mkstore: refusing to replace existing '$out'" >&2
    exit 1
fi

disk_sectors=131072 # 64 MiB
esp_start=2048
esp_end=$((disk_sectors - 2048 - 1))
esp_sectors=$((esp_end - esp_start + 1))
esp_offset=$((esp_start * 512))

mkdir -p "$(dirname "$out")"
tmp="$out.tmp.$$"
marker="$out.marker.$$"
trap 'rm -f "$tmp" "$marker"' EXIT INT TERM

truncate -s $((disk_sectors * 512)) "$tmp"
sgdisk --clear --new=1:"$esp_start":"$esp_end" --typecode=1:0700 \
    --change-name=1:"Cato Storage" "$tmp" >/dev/null

export MTOOLS_SKIP_CHECK=1
volume="$tmp@@$esp_offset"
mformat -i "$volume" -F -T "$esp_sectors" -H "$esp_start" -c 1 -v CATO_VM ::
mmd -i "$volume" ::/CATOOS
printf 'CATO-STORE-V1\nvirtual-drive\n' >"$marker"
mcopy -i "$volume" "$marker" ::/CATOOS/STORE.ID

mv "$tmp" "$out"
trap - EXIT INT TERM
rm -f "$marker"
