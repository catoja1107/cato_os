#!/usr/bin/env python3
"""Copies saved files between QEMU's virtual drive and the real Cato USB drive.

Usage: usb_sync.py <pull|push> <store.img>

QEMU cannot safely share a physical drive with Windows, so saved files are copied
at the start and end of a session. Only a valid copy with a newer generation
replaces an older one. The real drive is used only if it holds the Cato marker,
and only the two slot files in its CATOOS folder are ever written.
"""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

MOUNT = pathlib.Path("/mnt/e")
DRIVE = "E:"
MARKER = b"CATO-STORE-V1"
MAGIC = b"CATOFS01"
SLOTS = ("SLOTA.DAT", "SLOTB.DAT")
VOLUME_OFFSET = "@@1M"
MTOOLS_ENV = {**os.environ, "MTOOLS_SKIP_CHECK": "1"}


def generation(data):
    """Returns the generation of a valid slot image, or None."""
    if data is None or len(data) < 32 or data[:8] != MAGIC:
        return None
    number, payload, payload_crc = struct.unpack_from("<III", data, 8)
    header_crc = struct.unpack_from("<I", data, 24)[0]
    if zlib.crc32(data[:24]) != header_crc or payload > len(data) - 32:
        return None
    if zlib.crc32(data[32:32 + payload]) != payload_crc:
        return None
    return number


def has_marker(directory):
    try:
        with (directory / "STORE.ID").open("rb") as file:
            return file.read(len(MARKER)) == MARKER
    except OSError:
        return False


def usb_directory():
    """Returns the drive's CATOOS folder, or None if the Cato drive is not present."""
    directory = MOUNT / "CATOOS"
    if has_marker(directory):
        return directory

    command = ["mount", "-t", "drvfs", DRIVE, str(MOUNT)]
    if os.geteuid() != 0:
        command = ["sudo", "-n"] + command
    try:
        MOUNT.mkdir(exist_ok=True)
        subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    except OSError:
        return None
    return directory if has_marker(directory) else None


def read_usb(directory, name):
    try:
        return (directory / name).read_bytes()
    except OSError:
        return None


def write_usb(directory, name, data):
    """Replaces a slot file atomically, after verifying the written bytes."""
    if not has_marker(directory):
        raise OSError("the Cato marker is no longer present")
    temporary = directory / f"{name}.TMP"
    try:
        with temporary.open("wb") as file:
            file.write(data)
            file.flush()
            os.fsync(file.fileno())
        if temporary.read_bytes() != data:
            raise OSError("the data read back from USB did not match")
        os.replace(temporary, directory / name)
    finally:
        temporary.unlink(missing_ok=True)


def read_image(image, name):
    with tempfile.TemporaryDirectory(prefix="cato-sync-") as folder:
        target = pathlib.Path(folder) / name
        result = subprocess.run(
            ["mcopy", "-n", "-i", f"{image}{VOLUME_OFFSET}", f"::/CATOOS/{name}", str(target)],
            env=MTOOLS_ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
        )
        return target.read_bytes() if result.returncode == 0 and target.exists() else None


def write_image(image, name, data):
    with tempfile.TemporaryDirectory(prefix="cato-sync-") as folder:
        source = pathlib.Path(folder) / name
        source.write_bytes(data)
        subprocess.run(
            ["mcopy", "-o", "-i", f"{image}{VOLUME_OFFSET}", str(source), f"::/CATOOS/{name}"],
            env=MTOOLS_ENV, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=True,
        )


def is_newer(source, destination):
    source_generation = generation(source)
    if source_generation is None:
        return False
    destination_generation = generation(destination)
    return destination_generation is None or source_generation > destination_generation


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("pull", "push"):
        print(__doc__, file=sys.stderr)
        return 2

    action, image = sys.argv[1:]
    directory = usb_directory()
    if directory is None:
        print("USB sync: Cato USB drive not found, using the local copy only")
        return 0

    copied = 0
    for name in SLOTS:
        on_usb = read_usb(directory, name)
        on_image = read_image(image, name)
        if action == "pull" and is_newer(on_usb, on_image):
            write_image(image, name, on_usb)
            copied += 1
        elif action == "push" and is_newer(on_image, on_usb):
            write_usb(directory, name, on_image)
            copied += 1
    print(f"USB sync: {action} updated {copied} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
