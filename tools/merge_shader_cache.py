#!/usr/bin/env python3
"""Merge deko3d_shaders.bin files (the console's DKSH cache) into one prebuilt shader pack.

The console compiles each game shader variant the first time it is drawn (a stutter) and keeps it in
sdmc:/switch/pure-nx/deko3d_shaders.bin (src/renut_engine/gpu/banjo_renut_deko3d.cpp, LoadCache). The compiled code is
the same on every Switch, so caches played through on several consoles merge into one file that ships beside pure.nro:
a player who has it compiles only what nobody played yet.

    python tools/merge_shader_cache.py OUT.bin IN1.bin [IN2.bin ...]

Only caches of the same compiler (the tag in the header, kCacheTag) merge; a record that is not whole ends its file,
as on the console. Duplicate variants keep the first copy.
"""
import struct
import sys

HEAD = 24  # "BDKC", u32 version, 16-byte compiler tag
DKSH = 0x48534B44


def records(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HEAD or data[:4] != b"BDKC":
        sys.exit(f"{path}: not a deko3d_shaders.bin")
    tag = data[8:24]
    at, out = HEAD, []
    while at + 16 <= len(data):
        key, size, magic = struct.unpack_from("<QII", data, at)
        if size < 64 or size > (16 << 20) or at + 12 + size > len(data) or magic != DKSH:
            break
        out.append((key, data[at + 12:at + 12 + size]))
        at += 12 + size
    if at != len(data):
        print(f"{path}: {len(data) - at} bytes after the last whole record dropped")
    return data[:HEAD], tag, out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out_path, inputs = sys.argv[1], sys.argv[2:]
    header, tag, merged, seen = None, None, [], set()
    for path in inputs:
        h, t, recs = records(path)
        if tag is None:
            header, tag = h, t
        elif t != tag:
            sys.exit(f"{path}: another compiler's cache ({t.rstrip(bytes(1)).decode(errors='replace')}); not merged")
        new = 0
        for key, blob in recs:
            if key not in seen:
                seen.add(key)
                merged.append((key, blob))
                new += 1
        print(f"{path}: {len(recs)} variants, {new} new")
    with open(out_path, "wb") as f:
        f.write(header)
        for key, blob in merged:
            f.write(struct.pack("<QI", key, len(blob)))
            f.write(blob)
    print(f"{out_path}: {len(merged)} variants")


if __name__ == "__main__":
    main()
