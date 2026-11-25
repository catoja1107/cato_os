# Debugging

## Commands

Run these from the repo root in WSL Ubuntu.

| Command | Purpose |
| --- | --- |
| `make run` | Boot in a QEMU window with KVM when available. |
| `make debug` | Boot paused under TCG with a GDB stub on port 1234 and a CPU log in `build/qemu.log`. |
| `make test` | Run host unit tests and the headless QEMU smoke test. |
| `CATO_DISPLAY=none make debug` | Run the debug target without opening a window. |

## Attach GDB

Start `make debug`, then in a second terminal:

```sh
gdb -ex 'set architecture i386:x86-64' -ex 'target remote :1234'
```

The loader prints its `Image base`. A runtime address minus that base is the offset into `build/boot/BOOTX64.EFI`. The base can change between boots, so read it from the current run.

## Symptoms

**Firmware does not start the loader.** Confirm the file exists with `MTOOLS_SKIP_CHECK=1 mdir -i build/cato.img@@1M ::/EFI/BOOT`. In QEMU this shows as `BdsDxe: failed to load Boot0001 ... Not Found` followed by the UEFI shell. Firmware also reports "Not Found" when the file exists but is not a valid EFI executable. On real hardware, confirm that Secure Boot is off and that you chose the USB drive's UEFI boot entry.

**Build outputs filled with zeros.** If WSL stops before its writes to the Windows drive are flushed, build outputs can keep their size and timestamps but hold only zeros, so make never rebuilds them. Every `make` command except `make clean` first runs `scripts/check_build.py`. If it finds a damaged output, it deletes `build/` and the same command rebuilds everything.

**Black screen before ExitBootServices.** Check `build/serial.log` after `make smoke`. If the text is there, the problem is in the display path. Compare the printed resolution, pixel format, and scanline with what the drawing code uses.

**Black screen after ExitBootServices (Milestone 2 onward).** Firmware text output stops at that point, so only direct framebuffer writes still work. Paint a solid color immediately after the call. If the color appears, the exit worked and the fault is later. If it does not, check whether the call returned `EFI_INVALID_PARAMETER`, which means the memory map key is stale and the map must be fetched again.

**Triple fault or unexpected reset.** Run `make debug` and continue in GDB. A triple fault pauses the VM, and `build/qemu.log` records each exception. Look at the last `v=` vector: `0e` is a page fault with the faulting address in `CR2`, `0d` is a general protection fault, and `08` is a double fault.

**Page fault.** The error code bits are defined in Intel SDM Vol. 3A, "Interrupt 14: Page-Fault Exception". Use `info mem` and `info tlb` in the QEMU monitor to inspect the mappings.
