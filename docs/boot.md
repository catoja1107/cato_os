# Boot loader

## Milestone 1 design

**What:** A PE32+ UEFI application that finds the GOP framebuffer, prints firmware and display details, draws color bars directly to framebuffer memory, and waits for a key.

**Why:** It proves the toolchain, disk image, firmware entry, and display path before any kernel exists.

**Key structures:** `EFI_SYSTEM_TABLE`, `EFI_BOOT_SERVICES`, `EFI_GRAPHICS_OUTPUT_PROTOCOL`, and `struct framebuffer`.

**Trade-offs:** Text uses the firmware console until Milestone 3 adds a font renderer. The loader keeps the firmware's current video mode to avoid mode-switch failures. It tries the console's GOP first because the target PC has two GPUs.

## Hardware details

- **Entry ABI:** UEFI x86_64 uses the Microsoft x64 calling convention. Firmware passes the image handle in `RCX` and the system table in `RDX` (UEFI Specification, "Calling Conventions").
- **Red zone:** Disabled because firmware interrupt handlers can run on the loader's stack.
- **Watchdog:** Firmware arms a 5-minute watchdog before starting a boot application. The loader disables it with `SetWatchdogTimer` (UEFI Specification, "Boot Services").
- **Pixels:** Always address rows with `PixelsPerScanLine`, which can be larger than the visible width. Pixel layout comes from `PixelFormat` and `PixelInformation` (UEFI Specification, "Graphics Output Protocol").

## Deliberate shortcuts

- Only 32-bit-per-pixel framebuffers are supported.
- `PixelBltOnly` displays are reported as an error.
- Text output depends on the firmware console and ends at `ExitBootServices`.

## Boot splash

`scripts/mksplash.py` crops `assets/splash.png` to the cat's face, converts it to 8-bit grayscale, removes background noise, and fades the edges to black. The 700x600 result is compiled into the loader, which adds about 420 KB. The loader centers it in 92% of the screen with bilinear scaling and clears the rest of the display, including any vendor logo.

The loader stores the pixels uncompressed (deliberate shortcut). Scaling happens once per boot and uses integer math only.

**License note:** `assets/splash.png` was supplied by the project owner, who has confirmed the right to redistribute it. Its terms are separate from this repository's MIT license.

## Disk image

The image is a 64 MiB GPT disk with one 62 MiB FAT32 EFI System Partition starting at LBA 2048. The loader is stored at `\EFI\BOOT\BOOTX64.EFI`, the default removable-media path for x86_64 (UEFI Specification, "Removable Media Boot Behavior").
