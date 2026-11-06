#!/usr/bin/env python3
"""Count ARMv8.1+ instructions in the real code (__TEXT,__text and other
S_ATTR_PURE_INSTRUCTIONS sections) of every image in a split dyld shared cache.
Usage: isa-scan.py <dir with dyld_shared_cache_arm64*> [samples.bin]
Writes up to 16 sample instruction words per pattern to samples.bin
(u32 words, pattern order) for independent decoding with objdump.
"""
import glob, os, struct, sys
import numpy as np

PATTERNS = [
    ('LSE atomics ldadd/swp..', 0x3F208C00, 0x38200000),
    ('LSE cas',                 0x3FA07C00, 0x08A07C00),
    ('LSE casp',                0xBFA07C00, 0x08207C00),
    ('v8.1 RDM sqrdmlah/sh',    0xBF20F400, 0x2E008400),
    ('v8.2 dot product',        0xBF20FC00, 0x0E009400),
    ('v8.2 FP16 3-same',        0x9F60C400, 0x0E400400),
    ('v8.3 LDAPR (RCpc)',       0x3FFFFC00, 0x38BFC000),
    ('v8.3 FJCVTZS',            0xFFFFFC00, 0x1E7E0000),
]
S_ATTR_PURE_INSTRUCTIONS = 0x80000000


class Cache:
    def __init__(self, d):
        self.maps = []   # (addr, size, path, foff)
        self.files = {}
        main = None
        for p in sorted(glob.glob(os.path.join(d, 'dyld_shared_cache_arm64*'))):
            if p.endswith(('.symbols', '.map')):
                continue
            f = open(p, 'rb')
            hdr = f.read(0x200)
            if hdr[:4] != b'dyld':
                continue
            self.files[p] = f
            moff, mcnt = struct.unpack_from('<II', hdr, 0x10)
            f.seek(moff)
            for _ in range(mcnt):
                a, s, fo, mx, ini = struct.unpack('<QQQII', f.read(32))
                self.maps.append((a, s, p, fo))
            if p.endswith('dyld_shared_cache_arm64'):
                main = hdr
        io, ic = struct.unpack_from('<II', main, 0x1C0)
        if not ic:
            io, ic = struct.unpack_from('<II', main, 0x18)
        f = self.files[os.path.join(d, 'dyld_shared_cache_arm64')]
        f.seek(io)
        raw = f.read(32 * ic)
        self.images = []
        for i in range(ic):
            addr, _, _, poff = struct.unpack_from('<QQQI', raw, 32 * i)
            f.seek(poff)
            self.images.append((addr, f.read(256).split(b'\0')[0].decode()))

    def read(self, addr, n):
        for a, s, p, fo in self.maps:
            if a <= addr < a + s:
                f = self.files[p]
                f.seek(fo + addr - a)
                return f.read(min(n, a + s - addr))
        return b''


def main(d, sample_path=None):
    c = Cache(d)
    totals = {p[0]: 0 for p in PATTERNS}
    samples = {p[0]: [] for p in PATTERNS}
    per_image = {}
    words = 0
    for base, path in c.images:
        hdr = c.read(base, 32)
        if len(hdr) < 32 or struct.unpack_from('<I', hdr)[0] != 0xFEEDFACF:
            continue
        ncmds, sizeofcmds = struct.unpack_from('<II', hdr, 16)
        lc = c.read(base + 32, sizeofcmds)
        off = 0
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', lc, off)
            if cmd == 0x19:   # LC_SEGMENT_64
                nsects = struct.unpack_from('<I', lc, off + 64)[0]
                for s in range(nsects):
                    so = off + 72 + 80 * s
                    sectname = lc[so:so + 16].split(b'\0')[0]
                    addr, ssize = struct.unpack_from('<QQ', lc, so + 32)
                    flags = struct.unpack_from('<I', lc, so + 64)[0]
                    if not flags & S_ATTR_PURE_INSTRUCTIONS:
                        continue
                    data = c.read(addr, ssize)
                    w = np.frombuffer(data[:len(data) // 4 * 4], dtype='<u4')
                    words += w.size
                    for name, m, v in PATTERNS:
                        hits = np.nonzero((w & np.uint32(m)) == np.uint32(v))[0]
                        if hits.size:
                            totals[name] += hits.size
                            per_image.setdefault(path, {}).setdefault(name, 0)
                            per_image[path][name] += hits.size
                            for h in hits[:max(0, 16 - len(samples[name]))]:
                                samples[name].append((addr + 4 * int(h), int(w[h]), path, sectname.decode()))
            off += size
    print('images %d, code words scanned %d (%.0f MB)' % (len(c.images), words, words * 4 / 2**20))
    for name, _, _ in PATTERNS:
        n_img = sum(1 for v in per_image.values() if name in v)
        print('%-26s %9d hits in %4d images' % (name, totals[name], n_img))
    print('\nImages with hits (count per pattern):')
    for p, v in sorted(per_image.items(), key=lambda kv: -sum(kv[1].values())):
        print('%6d  %-70s %s' % (sum(v.values()), p, ', '.join('%s=%d' % (k.split()[0] + ' ' + k.split()[1], n) for k, n in v.items())))
    print('\nSamples (addr word image section):')
    for name, _, _ in PATTERNS:
        for a, wd, p, s in samples[name][:3]:
            print('  %-24s %#x %08x %s %s' % (name, a, wd, os.path.basename(p), s))
    if sample_path:
        with open(sample_path, 'wb') as f:
            for name, _, _ in PATTERNS:
                for a, wd, p, s in samples[name]:
                    f.write(struct.pack('<I', wd))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
