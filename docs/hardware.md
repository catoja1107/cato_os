# Hardware profile

Collected on 2026-11-23 from Windows with read-only queries. The Linux commands in the original plan have not been run on bare metal, so ACPI, APIC, IOMMU, and DIMM details are still unknown. Collect them from a Linux live USB before Milestone 5.

## System

| Item | Value |
| --- | --- |
| Board | ASUS PRIME X670E-PRO WIFI, BIOS 3040 |
| CPU | AMD Ryzen 9 7900X, 12 cores, 24 threads, x86_64 |
| Memory | 31.1 GiB visible to Windows |
| Firmware | UEFI |
| Secure Boot | Off |

## Devices and planned drivers

PCI IDs are `vendor:device`.

| Device | PCI ID | Plan |
| --- | --- | --- |
| NVIDIA GeForce RTX 4080, active at 3840x2160 | `10de:2704` | GOP framebuffer only (M1) |
| AMD Radeon integrated graphics | `1002:164e` | GOP framebuffer if firmware selects it |
| AMD xHCI controllers | `1022:15b6`, `1022:15b7`, `1022:15b8`, `1022:43f7` | xHCI driver (M9) |
| USB keyboards and receivers | USB `046d:c539`, `264a:3017` | USB HID boot keyboard (M9) |
| WD NVMe controllers, **internal** | `15b7:5030`, `15b7:5050` | NVMe, **read-only** (M9) |
| AMD SATA controllers | `1022:43f6` | AHCI, **read-only**, if needed (M9) |
| Realtek 2.5GbE | `10ec:8125` | Primary NIC driver (M9). Needs a connected cable. |
| MediaTek RZ608 Wi-Fi 6E | `14c3:0608` | Out of scope. Wi-Fi requires firmware and a wireless stack. |

## Storage safety

Both internal drives are 2 TB WD_BLACK NVMe disks (SN850X and SN8100) holding GPT partitions. Cato OS must never write to them. Disk drivers stay read-only until explicitly approved, and real-hardware boots use a removable USB drive only.

## Still needed

- ACPI tables and interrupt routing (`dmesg`, `acpidump`)
- DIMM configuration (`dmidecode -t memory`)
- Which GPU owns the UEFI console when both are enabled
- Whether the board exposes a usable serial port
