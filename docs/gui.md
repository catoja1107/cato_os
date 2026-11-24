# Cato Desktop

## Design

**What:** A standard desktop with icons, a bottom taskbar, a start menu, right-click menus, and overlapping windows that can be moved, resized, minimized, and maximized. It includes Files, a Text Editor, a Terminal, and a System info app, with the boot splash as the wallpaper.

**Why now:** It runs inside the UEFI loader because the kernel does not exist yet. Everything in `gui/` and `fs/` is plain C with no firmware calls, so it can move into the kernel once Milestones 2 to 9 provide memory, input, and scheduling.

**Key structures:** `struct canvas` is a software back buffer. `struct wm` holds windows, z-order, input state, app state, and a list of dirty rectangles. Only dirty rectangles are redrawn and copied to the framebuffer, because framebuffer memory is slow to write.

## Layers

| Path | Responsibility |
| --- | --- |
| `gui/canvas.c`, `gui/font.c`, `gui/icons.c` | Clipped drawing, the 5x7 font, and icons |
| `gui/wm.c` | Windows, focus, taskbar, start menu, pointer routing, and rendering |
| `gui/shell.c` | Desktop icons, file actions, right-click menus, and dialogs |
| `gui/app_files.c` | Files app |
| `gui/app_editor.c` | Text Editor |
| `gui/app_terminal.c` | Terminal and its commands |
| `gui/apps.c` | App registry and System app |
| `fs/fs.c` | [In-memory filesystem](filesystem.md) |
| `boot/desktop.c` | UEFI memory, input, clock, and display output |
| `boot/ps2_mouse.c` | Polled PS/2 mouse for virtual machines |

## Controls

| Input | Action |
| --- | --- |
| `F1` | Open the start menu |
| `Tab` | Switch windows |
| `F2` | Rename the selected item |
| `F3` | Move the focused window with the arrows. Enter finishes. |
| `F4` | Close the focused window |
| `F5` | Refresh Files |
| `F6` | Show the desktop |
| `Ctrl+N` | New folder |
| `Ctrl+C`, `Ctrl+X`, `Ctrl+V` | Copy, cut, paste |
| `Delete` | Move to the Trash |
| `Ctrl+S` | Save in the Text Editor |
| Double-click | Open an icon or file, or maximize a window from its title bar |
| Right-click | Open a menu for an item or empty space |

Terminal commands are `ls`, `cd`, `pwd`, `mkdir`, `touch`, `cat`, `edit`, `cp`, `mv`, `rm`, `files`, `editor`, `system`, `about`, `sysinfo`, `time`, `echo`, `clear`, `help`, and `exit`. Use double quotes for names with spaces.

## Input and hardware

OVMF does not expose a physical mouse to UEFI applications. Inside a VM, detected with CPUID.1:ECX bit 31, the desktop polls the emulated i8042 PS/2 mouse directly. On real hardware it never touches ports `0x60` or `0x64`. It uses the firmware's Simple Pointer Protocol if available, and otherwise stays keyboard-only.

`Ctrl` shortcuts rely on firmware reporting them as control codes. If a board does not, the Files toolbar and right-click menus provide the same actions.

The UI uses a whole-number scale of `min(width / 1280, height / 800)`, so a 3840x2160 display is laid out at 1920x1080 and drawn at 2x.

## Deliberate shortcuts

- It is single-tasking and runs on the firmware's timer until Milestone 7.
- Each app has one window.
- Files cannot be dragged between folders. Use cut and paste.
- The editor handles files up to 16 KiB and does not wrap lines.
- The Terminal runs built-in commands only until Milestone 11.
- Physical mouse support depends on the board's firmware.
- The desktop does no disk I/O and writes no NVRAM variables.
