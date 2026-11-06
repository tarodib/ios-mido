#!/usr/bin/env python3
"""Print the code-signature page size of dyld shared cache files.  cache-cdpage.py FILE..."""
import struct, sys
for path in sys.argv[1:]:
    with open(path, 'rb') as f:
        h = f.read(0x40)
        cs_off, cs_size = struct.unpack_from('<QQ', h, 0x28)
        f.seek(cs_off)
        sb = f.read(min(cs_size, 1 << 20))
    magic, length, count = struct.unpack_from('>III', sb, 0)
    out = []
    for i in range(count):
        typ, off = struct.unpack_from('>II', sb, 12 + 8 * i)
        m = struct.unpack_from('>I', sb, off)[0]
        if m == 0xfade0c02:                      # CodeDirectory
            ver = struct.unpack_from('>I', sb, off + 8)[0]
            hash_size, hash_type, platform, page_shift = sb[off + 36], sb[off + 37], sb[off + 38], sb[off + 39]
            out.append(f'CD type {typ:#x} v{ver:#x} hash {hash_type}/{hash_size} pageShift {page_shift}')
    print(f'{path.split("/")[-1]}: superblob {magic:#x} @ {cs_off:#x}: ' + '; '.join(out))
