#!/usr/bin/env python3
"""Symbolize addresses and find ARMv8.1+ instructions in an XNU kernel Mach-O.

  kscan.py KERNEL [addr ...]

- addr: prints the enclosing symbol (nearest preceding N_SECT symbol) + offset
- always: scans every S_ATTR_PURE_INSTRUCTIONS section for LSE atomics,
  groups hits by function and prints the functions (count, first address).
"""
import struct, sys
from collections import Counter, defaultdict

LSE = [('ld<op>/swp', 0x3F208C00, 0x38200000),
       ('cas', 0x3FA07C00, 0x08A07C00),
       ('casp', 0xBFA07C00, 0x08207C00)]


def load(path):
    buf = open(path, 'rb').read()
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<8I', buf, 0)
    assert magic == 0xfeedfacf
    off, sects, symtab = 32, [], None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', buf, off)
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', buf, off + 64)[0]
            for s in range(nsects):
                so = off + 72 + 80 * s
                name = buf[so:so + 16].split(b'\0')[0].decode()
                seg = buf[so + 16:so + 32].split(b'\0')[0].decode()
                addr, ssize, foff = struct.unpack_from('<QQI', buf, so + 32)
                sflags = struct.unpack_from('<I', buf, so + 64)[0]
                sects.append((seg, name, addr, ssize, foff, sflags))
        elif cmd == 0x2:
            symtab = struct.unpack_from('<4I', buf, off + 8)
        off += size
    syms = []
    if symtab:
        symoff, nsyms, stroff, strsize = symtab
        for i in range(nsyms):
            strx, ntype, nsect, desc, value = struct.unpack_from('<IBBHQ', buf, symoff + 16 * i)
            if (ntype & 0x0e) == 0x0e and value:          # N_SECT
                s = buf[stroff + strx:buf.index(b'\0', stroff + strx)].decode(errors='replace')
                syms.append((value, s))
    syms.sort()
    return buf, sects, syms


def sym_for(syms, addr):
    lo, hi = 0, len(syms)
    while lo < hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    if not lo:
        return '?'
    v, s = syms[lo - 1]
    return f'{s}+{addr - v:#x}'


def main():
    buf, sects, syms = load(sys.argv[1])
    print(f'{len(syms)} symbols')
    for a in sys.argv[2:]:
        a = int(a, 16)
        print(f'{a:#x}: {sym_for(syms, a)}')
    per_func = defaultdict(Counter)
    first = {}
    total = Counter()
    for seg, name, addr, size, foff, fl in sects:
        if not fl & 0x80000000:
            continue
        for i in range(0, size - size % 4, 4):
            w = struct.unpack_from('<I', buf, foff + i)[0]
            for k, m, v in LSE:
                if w & m == v:
                    pc = addr + i
                    f = sym_for(syms, pc).split('+')[0]
                    per_func[f][k] += 1
                    first.setdefault(f, pc)
                    total[k] += 1
    print('LSE in kernel code:', dict(total), 'in', len(per_func), 'functions')
    for f, c in sorted(per_func.items(), key=lambda kv: -sum(kv[1].values()))[:40]:
        print(f'  {sum(c.values()):5d}  {first[f]:#x}  {f}')


if __name__ == '__main__':
    main()
