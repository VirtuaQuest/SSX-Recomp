#!/usr/bin/env python3
"""
Extracts files from an Xbox 360 (or original Xbox) disc image using the XDVDFS
filesystem, so the executable can be passed to XenonAnalyse / XenonRecomp.

Usage:
    python extract_iso.py <image.iso> <output dir>              # default.xex only
    python extract_iso.py <image.iso> <output dir> --all        # every file
    python extract_iso.py <image.iso> <output dir> --file a/b   # specific file(s)
    python extract_iso.py <image.iso> --list                    # list contents
"""

import argparse
import os
import struct
import sys

SECTOR_SIZE = 0x800
MAGIC = b"MICROSOFT*XBOX*MEDIA"
VOLUME_DESCRIPTOR_SECTOR = 32
ATTRIBUTE_DIRECTORY = 0x10

# Game partition offsets for the known disc layouts.
KNOWN_PARTITIONS = [
    ("XGD3", 0x2080000),
    ("XGD2", 0xFD90000),
    ("XGD1", 0x18300000),
    ("Extracted XISO", 0x0),
]


class Entry:
    def __init__(self, path, sector, size, attributes):
        self.path = path
        self.sector = sector
        self.size = size
        self.attributes = attributes

    @property
    def is_directory(self):
        return (self.attributes & ATTRIBUTE_DIRECTORY) != 0


def has_magic(f, partition_offset):
    f.seek(partition_offset + VOLUME_DESCRIPTOR_SECTOR * SECTOR_SIZE)
    return f.read(len(MAGIC)) == MAGIC


def describe_non_xbox_image(f):
    f.seek(0x800)
    header = f.read(0x30)
    if header.startswith(b"PlayStation3"):
        title_id = header[0x10:0x30].split(b"\x00")[0].decode("ascii", "replace").strip()
        return f"this is a PlayStation 3 disc image ({title_id}), not an Xbox 360 one"
    f.seek(0x8000)
    if f.read(6)[1:6] == b"CD001":
        return "this is an ISO 9660 image, not an Xbox disc image"
    return "no XDVDFS volume descriptor was found"


def find_partition(f, file_size):
    for name, offset in KNOWN_PARTITIONS:
        if offset + (VOLUME_DESCRIPTOR_SECTOR + 1) * SECTOR_SIZE <= file_size and has_magic(f, offset):
            return name, offset

    # Fall back to scanning sector-aligned offsets for unusual layouts (e.g. trimmed or padded rips).
    chunk_size = 64 * 1024 * 1024
    position = 0
    while position < file_size:
        f.seek(position)
        chunk = f.read(chunk_size + len(MAGIC))
        index = chunk.find(MAGIC)
        while index != -1:
            absolute = position + index
            partition_offset = absolute - VOLUME_DESCRIPTOR_SECTOR * SECTOR_SIZE
            if absolute % SECTOR_SIZE == 0 and partition_offset >= 0:
                return "Unknown layout", partition_offset
            index = chunk.find(MAGIC, index + 1)
        position += chunk_size

    return None, None


def read_directory(f, partition_offset, sector, size, parent_path, entries):
    if size == 0:
        return

    f.seek(partition_offset + sector * SECTOR_SIZE)
    table = f.read(size)

    visited = set()
    stack = [0]
    while stack:
        offset = stack.pop()
        if offset in visited or offset + 14 > len(table):
            continue
        visited.add(offset)

        left, right, entry_sector, entry_size, attributes, name_length = struct.unpack_from("<HHIIBB", table, offset)
        if left == 0xFFFF and right == 0xFFFF:
            continue  # Padding at the end of a sector, or an empty directory.

        name = table[offset + 14:offset + 14 + name_length].decode("latin-1")
        if name in ("", ".", "..") or "/" in name or "\\" in name:
            raise ValueError(f"Invalid file name {name!r} in directory {parent_path!r}")

        path = f"{parent_path}/{name}" if parent_path else name
        entry = Entry(path, entry_sector, entry_size, attributes)
        entries.append(entry)

        if entry.is_directory:
            read_directory(f, partition_offset, entry_sector, entry_size, path, entries)

        if left not in (0, 0xFFFF):
            stack.append(left * 4)
        if right not in (0, 0xFFFF):
            stack.append(right * 4)


def read_filesystem(f):
    f.seek(0, os.SEEK_END)
    file_size = f.tell()

    layout, partition_offset = find_partition(f, file_size)
    if partition_offset is None:
        raise ValueError(f"Not an Xbox disc image: {describe_non_xbox_image(f)}.")

    f.seek(partition_offset + VOLUME_DESCRIPTOR_SECTOR * SECTOR_SIZE + len(MAGIC))
    root_sector, root_size = struct.unpack("<II", f.read(8))

    entries = []
    read_directory(f, partition_offset, root_sector, root_size, "", entries)
    return layout, partition_offset, entries


def extract_entry(f, partition_offset, entry, output_dir):
    destination = os.path.join(output_dir, *entry.path.split("/"))
    if entry.is_directory:
        os.makedirs(destination, exist_ok=True)
        return

    os.makedirs(os.path.dirname(destination), exist_ok=True)
    f.seek(partition_offset + entry.sector * SECTOR_SIZE)
    remaining = entry.size
    with open(destination, "wb") as out:
        while remaining > 0:
            data = f.read(min(remaining, 16 * 1024 * 1024))
            if not data:
                raise IOError(f"Unexpected end of image while reading {entry.path}")
            out.write(data)
            remaining -= len(data)


def main():
    parser = argparse.ArgumentParser(description="Extract files from an Xbox 360 disc image (XDVDFS).")
    parser.add_argument("image", help="Path to the .iso file")
    parser.add_argument("output", nargs="?", help="Output directory")
    parser.add_argument("--all", action="store_true", help="Extract every file instead of only default.xex")
    parser.add_argument("--file", action="append", default=[], help="Extract a specific file (path inside the image); can be repeated")
    parser.add_argument("--list", action="store_true", help="List the image contents and exit")
    args = parser.parse_args()

    with open(args.image, "rb") as f:
        try:
            layout, partition_offset, entries = read_filesystem(f)
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1

        print(f"Layout: {layout} (game partition at 0x{partition_offset:X}), {len(entries)} entries")

        if args.list:
            for entry in sorted(entries, key=lambda e: e.path.lower()):
                print(f"{'<DIR>' if entry.is_directory else entry.size:>12}  {entry.path}")
            return 0

        if not args.output:
            parser.error("an output directory is required unless --list is used")

        if args.all:
            selected = entries
        else:
            wanted = {p.replace("\\", "/").strip("/").lower() for p in (args.file or ["default.xex"])}
            selected = [e for e in entries if e.path.lower() in wanted]
            missing = wanted - {e.path.lower() for e in selected}
            if missing:
                print(f"error: not found in image: {', '.join(sorted(missing))}", file=sys.stderr)
                return 1

        for entry in selected:
            extract_entry(f, partition_offset, entry, args.output)
            if not entry.is_directory:
                print(f"Extracted {entry.path} ({entry.size} bytes)")

        xex_path = os.path.join(args.output, "default.xex")
        if os.path.isfile(xex_path):
            with open(xex_path, "rb") as xex:
                magic = xex.read(4)
            if magic != b"XEX2":
                print(f"warning: default.xex has unexpected magic {magic!r} (expected b'XEX2')", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
