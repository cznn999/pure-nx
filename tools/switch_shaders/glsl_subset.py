#!/usr/bin/env python3
"""A deko3d GLSL package (deko3d_glsl.bin, "BDKG" version 1) with only the listed shaders.

    python tools/switch_shaders/glsl_subset.py FULL.bin LIST.txt OUT.bin

LIST.txt: one container hash (16 hex digits) a line, '#' comments (tools/switch_shaders/runtime_shaders.txt). The entries
keep their compressed text and the package's dictionary; offsets are made again. The installer build (CMake
PURE_INSTALLER_BUILD) embeds the result in the NRO; the rest of the game's shaders are in the deko3d_glsl.bin the
installer makes beside it (banjo_renut_deko3d.cpp: that file first, then the NRO's).
"""
import struct
import sys

HEAD = '<4s7I'  # magic, version, count, dictSize, tableOffset, dataOffset, reserved[2]
ENTRY = '<QIIII8I'  # hash, offset, compressedSize, size, stage, boolMask[8]


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    full, listed, out = sys.argv[1:]
    data = open(full, 'rb').read()
    magic, version, count, dict_size, table, data_off, r0, r1 = struct.unpack_from(HEAD, data, 0)
    if magic != b'BDKG' or version != 1:
        sys.exit(f'{full}: not a GLSL package')
    want = set()
    for line in open(listed, encoding='utf-8'):
        line = line.split('#', 1)[0].split()
        if line:
            want.add(int(line[0], 16))
    es = struct.calcsize(ENTRY)
    entries, blobs, at = [], [], 0
    for i in range(count):
        e = list(struct.unpack_from(ENTRY, data, table + i * es))
        if e[0] not in want:
            continue
        blob = data[data_off + e[1]:data_off + e[1] + e[2]]
        e[1] = at
        at += len(blob)
        entries.append(e)
        blobs.append(blob)
    missing = want - {e[0] for e in entries}
    if missing:
        sys.exit(f'{full}: {len(missing)} listed shaders not in the package: ' +
                 ' '.join(f'{h:016X}' for h in sorted(missing)))
    hs = struct.calcsize(HEAD)
    dictionary = data[hs:hs + dict_size]
    new_table = (hs + dict_size + 15) & ~15
    new_data = (new_table + len(entries) * es + 15) & ~15
    buf = bytearray(new_data)
    struct.pack_into(HEAD, buf, 0, b'BDKG', 1, len(entries), dict_size, new_table, new_data, 0, 0)
    buf[hs:hs + dict_size] = dictionary
    for i, e in enumerate(entries):
        struct.pack_into(ENTRY, buf, new_table + i * es, *e)
    buf += b''.join(blobs)
    open(out, 'wb').write(buf)
    print(f'{out}: {len(entries)} of {count} shaders, {len(buf)} bytes')


if __name__ == '__main__':
    main()
