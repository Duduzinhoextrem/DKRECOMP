"""Read generated RomFS through its hash chains and directory links, independently of the builder's records."""
from pathlib import Path
import os
import struct
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))

from dkrecomp.romfs import build_romfs


def read_image(path):
    data = path.read_bytes()
    size, dh, dhs, dt, dts, fh, fhs, ft, fts, payload = struct.unpack_from("<10Q", data)
    assert size == 80 and payload == 0x200 and dh >= payload and ft + fts == len(data)
    dirs, files = {}, {}
    for table, length, fmt, output in ((dt, dts, "<6I", dirs), (ft, fts, "<IIQQII", files)):
        offset = 0
        record_size = struct.calcsize(fmt)
        while offset < length:
            record = struct.unpack_from(fmt, data, table + offset)
            name = data[table + offset + record_size:table + offset + record_size + record[-1]]
            output[offset] = (record, name)
            offset += (record_size + record[-1] + 3) // 4 * 4
        assert offset == length

    def lookup(parent, name, is_dir):
        start, count, records = (dh, dhs // 4, dirs) if is_dir else (fh, fhs // 4, files)
        h = parent ^ 123456789
        for byte in name:
            h = ((h // 32) + (h % 32) * 2**27) ^ byte
        offset, = struct.unpack_from("<I", data, start + 4 * (h % count))
        seen = set()
        while offset != 0xffffffff:
            assert offset not in seen
            seen.add(offset)
            record, entry_name = records[offset]
            if record[0] == parent and entry_name == name:
                return offset
            offset = record[-2]
        raise FileNotFoundError(name)

    found = {}

    def walk(offset, parent_path):
        record, _ = dirs[offset]
        child = record[2]
        while child != 0xffffffff:
            directory, name = dirs[child]
            assert directory[0] == offset and lookup(offset, name, True) == child
            walk(child, parent_path / name.decode("utf-8"))
            child = directory[1]
        child = record[3]
        while child != 0xffffffff:
            file_record, name = files[child]
            assert file_record[0] == offset and lookup(offset, name, False) == child
            start, length = file_record[2:4]
            assert start % 16 == 0 and payload + start + length <= dh
            found[(parent_path / name.decode("utf-8")).as_posix()] = data[payload + start:payload + start + length]
            child = file_record[1]

    assert dirs[0][0][0] == 0
    walk(0, Path())
    assert len(found) == len(files)
    return found


def test_tree_hashes_siblings_utf8_and_empty_files(tmp_path):
    source = tmp_path / "source"
    (source / "a" / "empty").mkdir(parents=True)
    (source / "b").mkdir()
    expected = {"a/same": b"first", "b/same": b"second", "zero": b"", "café": b"utf8"}
    # Enough names to exercise hash collisions as well as sibling links.
    expected.update({f"f{i:03d}": bytes([i]) * (i + 1) for i in range(100)})
    for name, data in expected.items():
        (source / name).write_bytes(data)
    output = tmp_path / "romfs.bin"
    result = build_romfs(source, output)
    assert result["directories"] == 4 and result["files"] == len(expected)
    assert result["file_bytes"] == sum(map(len, expected.values()))
    assert result["image_bytes"] == output.stat().st_size
    assert read_image(output) == expected


def test_empty_and_deterministic(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    first, second = tmp_path / "one.bin", tmp_path / "two.bin"
    build_romfs(source, first)
    assert read_image(first) == {}
    for name in ("z", "a", "q"):
        (source / name).write_bytes(name.encode())
    build_romfs(source, first, overwrite=True)
    build_romfs(source, second)
    assert first.read_bytes() == second.read_bytes()
    with pytest.raises(FileExistsError):
        build_romfs(source, first)


def test_output_must_not_be_an_input(tmp_path):
    with pytest.raises(ValueError, match="outside"):
        build_romfs(tmp_path, tmp_path / "romfs.bin")


def test_symlinks_are_rejected(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    outside = tmp_path / "outside"
    outside.write_bytes(b"not in the filesystem")
    try:
        (source / "link").symlink_to(outside)
    except OSError:
        pytest.skip("symlink creation is unavailable on this host")
    with pytest.raises(ValueError, match="links"):
        build_romfs(source, tmp_path / "romfs.bin")
