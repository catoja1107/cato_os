# Cato OS

Cato OS is a from-scratch, 64-bit, UEFI-only, monolithic Unix-like operating system for x86_64, written in C and assembly.

**Status:** Milestone 1 plus a desktop preview. A custom UEFI loader boots in QEMU with OVMF, shows a boot splash, and runs Cato Desktop, with windows, a file manager, a text editor, and a terminal. **Files are stored in RAM and are lost on exit or reboot.** The kernel starts at Milestone 2.

## Repository layout

| Path | Contents |
| --- | --- |
| `boot/` | UEFI loader, built as `BOOTX64.EFI` |
| `gui/` | Cato Desktop: drawing, font, window manager, shell, and apps |
| `fs/` | In-memory filesystem |
| `assets/` | Boot splash source image. See the license note in [docs/boot.md](docs/boot.md). |
| `scripts/` | Toolchain setup, disk image builder, QEMU launcher |
| `tests/` | QEMU smoke test and host unit tests |
| `docs/` | Subsystem designs, debugging guide, hardware profile |
| `x86/` | Earlier 32-bit BIOS prototype. It is not part of this build. |

## Setup

The build runs in WSL Ubuntu 24.04. From PowerShell, open a shell in the repo:

```powershell
wsl -d Ubuntu --cd C:\Users\jcato\git\cato_os
```

Then install the toolchain once:

```sh
sh scripts/setup-wsl.sh
```

## Build and run

```sh
make run     # build and boot in a QEMU window
make test    # run unit tests and the headless QEMU smoke test
make debug   # boot paused with a GDB stub on port 1234
make clean   # remove build output
```

The QEMU window shows the boot splash for 3 seconds, then opens the desktop. Press any key to skip the wait, or `D` for the diagnostics screen first. On the desktop, double-click icons, right-click for menus, and use `F1` for the start menu and `Tab` to switch windows. Click inside the QEMU window to use the mouse, and press `Ctrl+Alt+G` to release your keyboard and mouse. See [docs/gui.md](docs/gui.md) for all controls. See [docs/debugging.md](docs/debugging.md) for troubleshooting.

## Real hardware

```sh
make usb IMG=build/cato-usb.img
```

This creates an image **file** and never writes to a device. It refuses device paths and any output name that does not end in `.img`.

**Do not flash yet.** Real-hardware bring-up is Milestone 12. Writing an image erases the target drive, so that step will include a procedure for positively identifying the USB drive first.

## Documentation

- [Boot loader design](docs/boot.md)
- [Desktop design](docs/gui.md)
- [In-memory filesystem](docs/filesystem.md)
- [Debugging guide](docs/debugging.md)
- [Hardware profile](docs/hardware.md)

## License

See [LICENSE](LICENSE) for details.
