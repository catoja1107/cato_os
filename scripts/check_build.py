#!/usr/bin/env python3
"""Finds damaged build outputs and deletes build/ so make rebuilds everything.

WSL can lose writes to the Windows drive that were not yet flushed when it
stopped. The result is a file of the right size that holds only zeros. Its
timestamp is fresh, so make treats it as up to date and never rebuilds it.
The Makefile runs this check before every build.
"""
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"

# Outputs whose first bytes are never legitimately zero.
CHECKED_SUFFIXES = {".o", ".efi", ".d", ".c"}

# Each boot image and the loader it must contain. mkimage.sh puts the ESP at 1 MiB.
IMAGES = {
    BUILD / "cato.img": BUILD / "boot" / "BOOTX64.EFI",
    BUILD / "test" / "cato-test.img": BUILD / "test" / "BOOTX64.EFI",
}


def damaged_file(path):
    """Returns why a compiler or linker output is damaged, or None."""
    with open(path, "rb") as f:
        head = f.read(4096)
    if not head:
        return "is empty"
    if not any(head):
        return "holds only zeros"
    if path.suffix.lower() == ".efi" and head[:2] != b"MZ":
        return "is not an EFI executable"
    return None


def damaged_image(image, loader):
    """Returns why a boot image that make considers up to date is damaged, or None."""
    if not loader.is_file() or image.stat().st_mtime < loader.stat().st_mtime:
        return None  # make rebuilds this image anyway.
    try:
        inside = subprocess.run(
            ["mcopy", "-n", "-i", f"{image}@@1M", "::/EFI/BOOT/BOOTX64.EFI", "-"],
            env={**os.environ, "MTOOLS_SKIP_CHECK": "1"},
            capture_output=True,
            check=False,
        )
    except FileNotFoundError:
        return None  # Without mtools the image cannot be checked.
    if inside.returncode != 0:
        return "has no readable EFI/BOOT/BOOTX64.EFI"
    if inside.stdout != loader.read_bytes():
        return f"does not contain the current {loader.relative_to(REPO)}"
    return None


def find_damage():
    # Each file access on the Windows drive is a slow round trip, so walk without
    # a stat per file and read the files in parallel.
    paths = [
        Path(root, name)
        for root, _, names in os.walk(BUILD)
        for name in names
        if Path(name).suffix.lower() in CHECKED_SUFFIXES
    ]
    with ThreadPoolExecutor(max_workers=16) as pool:
        for path, reason in zip(paths, pool.map(damaged_file, paths)):
            if reason:
                return path, reason
    for image, loader in IMAGES.items():
        if image.is_file():
            reason = damaged_image(image, loader)
            if reason:
                return image, reason
    return None


def main():
    if not BUILD.is_dir():
        return 0
    damage = find_damage()
    if damage is None:
        return 0

    path, reason = damage
    print(f"check_build: {path.relative_to(REPO)} {reason}.", file=sys.stderr)
    print("check_build: deleting build/ so everything is rebuilt.", file=sys.stderr)
    try:
        shutil.rmtree(BUILD)
    except OSError as error:
        print(f"check_build: could not delete build/: {error}", file=sys.stderr)
        print("check_build: close anything using build/ (such as QEMU), then run 'make clean'.",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
