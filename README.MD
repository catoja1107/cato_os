# Cato OS

Cato OS is an experimental operating system project targeting **x86**

## Features

- Written for x86 platforms (no ARM or other architectures supported)
- Minimal bootloader and kernel

## Setup

### Prerequisites

- An x86-compatible machine or emulator (e.g., QEMU, VirtualBox, Bochs)
- GCC cross-compiler for i386 (`i386-elf-gcc`)
- `make` and `nasm` installed

### Running (with QEMU)

```sh
qemu-system-i386 -kernel build/kernel.bin
```

## Notes

- For development, using a virtual machine or emulator is recommended.

## License

See [LICENSE](LICENSE) for details.