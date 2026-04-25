#!/usr/bin/env python3
"""
pack-initramfs.py — emit a cpio-newc archive from a list of files.

Slice 7.6b.  The archive lands as a binary blob inside `kernel-test.bin`'s
.rodata via `.incbin`; the slice-7.6b ramfs slurp parses it at component
init() and seeds itself with the embedded files.

Format reference: cpio-newc (also called "new ascii" or "070701").
busybox + Linux's initramfs both speak it natively, so once 7.6c/d
land we don't have to rev the on-disk format.

Each entry layout:

  +-----------------+
  |   110-byte hdr  |   13 ASCII-hex fields + 6-byte magic
  +-----------------+
  |   name + NUL    |   `c_namesize` bytes, then padded so the next
  |                 |   field starts on a 4-byte boundary.
  +-----------------+
  |   file data     |   `c_filesize` bytes, then padded to 4 bytes.
  +-----------------+

Trailer entry: name `TRAILER!!!`, file_size 0, c_nlink 1.

Usage:
    pack-initramfs.py OUT_BIN ENTRY ...

Each ENTRY is `path-on-disk:archive-name` (e.g.
`test/kernel/init_prog.elf:/init`).  The archive name must start with `/`
(matches our ramfs's absolute-path-only world); leading `/`s are stripped
when packing because cpio-newc convention is unrooted names, but the
parser side prepends `/` back so paths look POSIX-ish in the live FS.
"""

import os
import sys


MAGIC = b"070701"


def hex8(n: int) -> bytes:
    """Format an integer as 8 ASCII-hex bytes — cpio-newc field width."""
    if n < 0 or n > 0xFFFFFFFF:
        raise ValueError(f"value {n} out of range for cpio-newc field")
    return f"{n:08X}".encode("ascii")


def pad4(buf: bytearray) -> None:
    """Append zero bytes to bring `buf` to a 4-byte boundary."""
    while len(buf) % 4:
        buf.append(0)


def append_entry(buf: bytearray, name: str, data: bytes,
                 mode: int, ino: int) -> None:
    """Append one cpio-newc record (header + name + data + padding)."""
    name_bytes = name.encode("utf-8") + b"\x00"   # cpio name is NUL-terminated
    namesize   = len(name_bytes)
    filesize   = len(data)

    buf.extend(MAGIC)
    buf.extend(hex8(ino))         # c_ino
    buf.extend(hex8(mode))        # c_mode (0o100644 = regular file rw-r--r--)
    buf.extend(hex8(0))           # c_uid
    buf.extend(hex8(0))           # c_gid
    buf.extend(hex8(1))           # c_nlink (1 for regular files)
    buf.extend(hex8(0))           # c_mtime — fixed for reproducibility
    buf.extend(hex8(filesize))    # c_filesize
    buf.extend(hex8(0))           # c_devmajor
    buf.extend(hex8(0))           # c_devminor
    buf.extend(hex8(0))           # c_rdevmajor
    buf.extend(hex8(0))           # c_rdevminor
    buf.extend(hex8(namesize))    # c_namesize
    buf.extend(hex8(0))           # c_check (always 0 for newc)

    buf.extend(name_bytes)
    pad4(buf)

    buf.extend(data)
    pad4(buf)


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    out_path = argv[1]
    entries  = argv[2:]

    blob = bytearray()
    ino  = 1
    for entry in entries:
        if ":" not in entry:
            print(f"pack-initramfs.py: malformed entry '{entry}' "
                  f"(expected path:name)", file=sys.stderr)
            return 1
        path, name = entry.split(":", 1)
        if not name.startswith("/"):
            print(f"pack-initramfs.py: archive name '{name}' must start "
                  f"with '/'", file=sys.stderr)
            return 1

        with open(path, "rb") as f:
            data = f.read()

        # Strip the leading '/' for the on-disk record; the parser side
        # prepends '/' back when calling vfs_simple's create.  This
        # matches busybox/Linux convention.
        archive_name = name.lstrip("/")
        append_entry(blob, archive_name, data, 0o100644, ino)
        ino += 1

    # Trailer entry — the parser stops scanning when it sees this name.
    append_entry(blob, "TRAILER!!!", b"", 0o000000, 0)

    with open(out_path, "wb") as f:
        f.write(blob)

    print(f"pack-initramfs.py: wrote {out_path} "
          f"({len(blob)} bytes, {len(entries)} entries + trailer)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
