"""Build a raw, unencrypted Switch RomFS image from files already extracted by the user.

The metadata layout and path hash are described by libnx's romfs_dev.h / romfs_dev.c.
This writes the filesystem itself, without an NCA or its integrity/encryption layers.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import struct
import tempfile

NONE = 0xFFFFFFFF
HEADER = struct.Struct("<10Q")
DIR = struct.Struct("<6I")
FILE = struct.Struct("<IIQQII")


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & -alignment


def path_hash(parent: int, name: bytes) -> int:
    value = parent ^ 123456789
    for byte in name:
        value = ((value >> 5) | (value << 27)) & 0xFFFFFFFF
        value ^= byte
    return value


def bucket_count(entries: int) -> int:
    # Prime-sized tables keep chains short; even an empty filesystem needs a hash table.
    n = max(3, entries | 1)
    while any(n % divisor == 0 for divisor in range(3, int(n ** 0.5) + 1, 2)):
        n += 2
    return n


@dataclass
class Directory:
    path: Path
    name: bytes
    parent: Directory | None = None
    dirs: list[Directory] = field(default_factory=list)
    files: list[FileEntry] = field(default_factory=list)
    offset: int = 0
    sibling: int = NONE


@dataclass
class FileEntry:
    path: Path
    name: bytes
    parent: Directory
    size: int
    mtime_ns: int
    offset: int = 0
    data_offset: int = 0
    sibling: int = NONE


def build_romfs(source: str | Path, output: str | Path, *, overwrite: bool = False) -> dict:
    root_path = Path(source).resolve(strict=True)
    output = Path(output).resolve()
    if not root_path.is_dir():
        raise ValueError("RomFS source must be a directory")
    if output.is_relative_to(root_path):
        raise ValueError("RomFS output must be outside the source directory")
    if output.exists() and not overwrite:
        raise FileExistsError(f"output already exists: {output} (use --force to replace it)")
    dirs: list[Directory] = []
    files: list[FileEntry] = []

    def scan(directory: Directory) -> None:
        dirs.append(directory)
        for path in sorted(directory.path.iterdir(), key=lambda p: p.name.encode("utf-8")):
            if path.is_symlink() or path.is_junction():
                raise ValueError(f"links are not supported in RomFS: {path}")
            name = path.name.encode("utf-8")
            if path.is_dir():
                child = Directory(path, name, directory)
                directory.dirs.append(child)
                scan(child)
            elif path.is_file():
                stat = path.stat()
                entry = FileEntry(path, name, directory, stat.st_size, stat.st_mtime_ns)
                directory.files.append(entry)
                files.append(entry)
            else:
                raise ValueError(f"not a regular file or directory: {path}")

    scan(Directory(root_path, b""))
    dir_size = file_size = data_size = 0
    for entry in dirs:
        entry.offset = dir_size
        dir_size += align(DIR.size + len(entry.name), 4)
    for entry in files:
        entry.offset = file_size
        file_size += align(FILE.size + len(entry.name), 4)
        entry.data_offset = align(data_size, 16)
        data_size = entry.data_offset + entry.size
    if max(dir_size, file_size) >= NONE:
        raise ValueError("RomFS metadata exceeds the 32-bit offset range")
    for directory in dirs:
        for children in (directory.dirs, directory.files):
            for first, second in zip(children, children[1:]):
                first.sibling = second.offset

    dir_buckets = [NONE] * bucket_count(len(dirs))
    file_buckets = [NONE] * bucket_count(len(files))
    dir_table = bytearray(dir_size)
    file_table = bytearray(file_size)
    for entry in dirs:
        parent = entry.parent.offset if entry.parent else 0
        bucket = path_hash(parent, entry.name) % len(dir_buckets)
        DIR.pack_into(dir_table, entry.offset, parent, entry.sibling,
                      entry.dirs[0].offset if entry.dirs else NONE,
                      entry.files[0].offset if entry.files else NONE,
                      dir_buckets[bucket], len(entry.name))
        dir_table[entry.offset + DIR.size:entry.offset + DIR.size + len(entry.name)] = entry.name
        dir_buckets[bucket] = entry.offset
    for entry in files:
        bucket = path_hash(entry.parent.offset, entry.name) % len(file_buckets)
        FILE.pack_into(file_table, entry.offset, entry.parent.offset, entry.sibling,
                       entry.data_offset, entry.size, file_buckets[bucket], len(entry.name))
        file_table[entry.offset + FILE.size:entry.offset + FILE.size + len(entry.name)] = entry.name
        file_buckets[bucket] = entry.offset
    dir_hash = struct.pack(f"<{len(dir_buckets)}I", *dir_buckets)
    file_hash = struct.pack(f"<{len(file_buckets)}I", *file_buckets)
    # Switch images start file data at 0x200 and keep the tables after it, rather than before it.
    data_off = 0x200
    dir_hash_off = align(data_off + data_size, 4)
    dir_table_off = dir_hash_off + len(dir_hash)
    file_hash_off = dir_table_off + len(dir_table)
    file_table_off = file_hash_off + len(file_hash)
    image_size = file_table_off + len(file_table)
    header = HEADER.pack(HEADER.size, dir_hash_off, len(dir_hash), dir_table_off, len(dir_table),
                         file_hash_off, len(file_hash), file_table_off, len(file_table), data_off)

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(mode="wb", dir=output.parent, prefix=output.name + ".",
                                         suffix=".tmp", delete=False) as image:
            temporary = Path(image.name)
            image.write(header)
            for entry in files:
                image.seek(data_off + entry.data_offset)
                with entry.path.open("rb") as src:
                    remaining = entry.size
                    while remaining:
                        chunk = src.read(min(remaining, 4 * 1024 * 1024))
                        if not chunk:
                            raise ValueError(f"file shrank during RomFS generation: {entry.path}")
                        image.write(chunk)
                        remaining -= len(chunk)
                    if src.read(1) or os.fstat(src.fileno()).st_mtime_ns != entry.mtime_ns:
                        raise ValueError(f"file changed during RomFS generation: {entry.path}")
            image.seek(dir_hash_off)
            for section in (dir_hash, dir_table, file_hash, file_table):
                image.write(section)
            image.truncate(image_size)
        os.replace(temporary, output)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return {"directories": len(dirs), "files": len(files), "file_bytes": sum(f.size for f in files),
            "image_bytes": image_size, "output": str(output)}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="directory of already-extracted files")
    parser.add_argument("output", help="raw RomFS image (outside the source directory)")
    parser.add_argument("--force", action="store_true", help="replace an existing output after success")
    args = parser.parse_args(argv)
    print(json.dumps(build_romfs(args.source, args.output, overwrite=args.force), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
