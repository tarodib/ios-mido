#!/usr/bin/env python3
"""Minimal FDT reader: dtdump.py <dtb> [path-substring ...]
Prints every node whose path contains one of the substrings, with its
properties decoded as strings (if printable) or big-endian u32 cells."""
import struct, sys


def parse(blob):
    magic, total, off_struct, off_strings = struct.unpack_from('>4I', blob, 0)
    assert magic == 0xd00dfeed, 'not an FDT'
    nodes, path, p = [], [], off_struct
    while True:
        tok, = struct.unpack_from('>I', blob, p); p += 4
        if tok == 1:  # BEGIN_NODE
            end = blob.index(b'\0', p); name = blob[p:end].decode(); p = (end + 4) & ~3
            path.append(name); nodes.append(['/'.join(path) or '/', {}])
        elif tok == 2:  # END_NODE
            path.pop()
        elif tok == 3:  # PROP
            ln, no = struct.unpack_from('>II', blob, p); p += 8
            name = blob[off_strings + no:blob.index(b'\0', off_strings + no)].decode()
            stack_path = '/'.join(path) or '/'
            for n in reversed(nodes):
                if n[0] == stack_path:
                    n[1][name] = blob[p:p + ln]; break
            p = (p + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            return nodes


def fmt(v):
    if v and v[-1] == 0 and all(32 <= c < 127 or c == 0 for c in v) and v[0] != 0:
        return ' | '.join(s.decode() for s in v.split(b'\0') if s)
    if len(v) % 4 == 0:
        return '<' + ' '.join('%#x' % c for c in struct.unpack('>%dI' % (len(v) // 4), v)) + '>'
    return v.hex()


if __name__ == '__main__':
    nodes = parse(open(sys.argv[1], 'rb').read())
    pats = sys.argv[2:]
    for path, props in nodes:
        if not pats or any(s in path for s in pats):
            print(path)
            for k, v in props.items():
                print('   %s = %s' % (k, fmt(v)[:300]))
