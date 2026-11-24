# In-memory filesystem

## Design

**What:** A small hierarchical filesystem in `fs/` that backs the desktop, Files, the Text Editor, and the Terminal.

**Why RAM only:** The project's safety rules keep real disks read-only until explicitly approved. This filesystem never calls a disk driver or firmware file protocol, so it cannot change anything on a real drive. **All files are lost when you exit or reboot.**

**Key structures:** A fixed table of 256 `fs_node` entries linked as a parent, first-child, and next-sibling tree. File data lives in a pool of 4,096 blocks of 512 bytes (2 MiB), chained through `block_next`.

## Behavior

| Operation | Rule |
| --- | --- |
| Create | Names must be unique in a folder, at most 31 characters, and cannot contain `/` or `"`. |
| Write | Replaces the whole file. A write that cannot fit leaves the old contents intact. |
| Copy | Copies whole trees. Capacity is checked first, so a failed copy leaves nothing behind. |
| Move | A name collision produces a unique name, such as `Report (2).txt`. Nothing is overwritten. |
| Trash | Records the original folder and name. |
| Restore | Returns the item to its folder, or to Documents if that folder is gone. |
| Delete | Permanent. Only offered for items already in the Trash. |

`Desktop`, `Documents`, `Downloads`, `Pictures`, and `Trash` are protected and cannot be renamed, moved, or deleted.

## Deliberate shortcuts

- There is no persistence.
- There are no permissions, links, or timestamps other than modified time.
- Limits are fixed at compile time.
- This is not the Milestone 10 VFS. It can become that layer's `tmpfs`.
