"""LZ4 block decompression. Uses the `lz4` package when installed, else a pure-Python decoder (slow but exact).

Format: lz4.org "LZ4 Block Format Description".
"""
from __future__ import annotations

try:  # optional fast path
    import lz4.block as _lz4  # type: ignore
except Exception:  # pragma: no cover - depends on environment
    _lz4 = None


class Lz4Error(ValueError):
    pass


def decompress_py(src: bytes, size: int) -> bytes:
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                if i >= n:
                    raise Lz4Error("truncated literal length")
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        if i + lit > n:
            raise Lz4Error("literals past end of input")
        out += src[i:i + lit]
        i += lit
        if i >= n:
            break  # last sequence has literals only
        if i + 2 > n:
            raise Lz4Error("truncated match offset")
        off = src[i] | (src[i + 1] << 8)
        i += 2
        if off == 0 or off > len(out):
            raise Lz4Error(f"bad match offset {off}")
        mlen = token & 15
        if mlen == 15:
            while True:
                if i >= n:
                    raise Lz4Error("truncated match length")
                b = src[i]
                i += 1
                mlen += b
                if b != 255:
                    break
        mlen += 4
        start = len(out) - off
        if off >= mlen:
            out += out[start:start + mlen]
        else:  # overlapping copy
            for k in range(mlen):
                out.append(out[start + k])
        if len(out) > size:
            raise Lz4Error("output larger than declared size")
    if len(out) != size:
        raise Lz4Error(f"decompressed {len(out)} bytes, expected {size}")
    return bytes(out)


def decompress(src: bytes, size: int) -> bytes:
    if _lz4 is not None:
        return _lz4.decompress(src, uncompressed_size=size)
    return decompress_py(src, size)
