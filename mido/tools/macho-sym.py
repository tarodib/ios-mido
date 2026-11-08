#!/usr/bin/env python3
"""Symbolicate addresses in a 64-bit Mach-O: macho-sym.py FILE MH_LOADED ADDR..."""
import struct, sys, bisect
d = open(sys.argv[1], 'rb').read()
ncmds = struct.unpack_from('<I', d, 16)[0]
o = 32; base = 0; symoff = nsyms = stroff = 0
for _ in range(ncmds):
    c, s = struct.unpack_from('<II', d, o)
    if c == 0x19 and d[o+8:o+24].rstrip(b'\0') == b'__TEXT':
        base = struct.unpack_from('<Q', d, o+24)[0]
    if c == 2:
        symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', d, o+8)
    o += s
syms = []
for i in range(nsyms):
    sx, t, sect, desc, v = struct.unpack_from('<IBBHQ', d, symoff + 16 * i)
    if t & 0xe0 or not sect: continue
    e = d.index(b'\0', stroff + sx)
    syms.append((v, d[stroff + sx:e].decode('latin1')))
syms.sort(); addrs = [s[0] for s in syms]
mh = int(sys.argv[2], 16)
for a in sys.argv[3:]:
    v = int(a, 16) - mh + base
    i = bisect.bisect_right(addrs, v) - 1
    print(a, syms[i][1] if i >= 0 else '?', '+', hex(v - addrs[i]) if i >= 0 else '')
