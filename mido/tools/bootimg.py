#!/usr/bin/env python3
"""Android boot.img v0 tool for LK (aboot) devices.
  bootimg.py info <boot.img>
  bootimg.py unpack <boot.img> <outdir>
  bootimg.py pack --kernel K [--ramdisk R] [--cmdline C] -o out.img   (mido defaults)
"""
import argparse, hashlib, os, struct

HDR = '<8s10I16s512s32s1024s'


def info(path, outdir=None):
    d = open(path, 'rb').read()
    (magic, ks, ka, rs, ra, ss, sa, ta, ps, dts, osv, name, cmd, _id, xcmd) = struct.unpack_from(HDR, d, 0)
    print('magic', magic, 'page', ps)
    print('kernel  size %#x addr %#x' % (ks, ka))
    print('ramdisk size %#x addr %#x' % (rs, ra))
    print('second  size %#x addr %#x  tags %#x  dt_size %#x' % (ss, sa, ta, dts))
    print('cmdline', (cmd.split(b'\0')[0] + xcmd.split(b'\0')[0]).decode())
    pg = lambda n: (n + ps - 1) // ps * ps
    off = ps
    k = d[off:off + ks]; off += pg(ks)
    r = d[off:off + rs]; off += pg(rs)
    s = d[off:off + ss]; off += pg(ss)
    t = d[off:off + dts]
    print('kernel: gzip' if k[:2] == b'\x1f\x8b' else 'kernel: ARM64 Image' if k[0x38:0x3c] == b'ARMd' else 'kernel: ?',
          '| appended FDTs:', k.count(b'\xd0\x0d\xfe\xed'), '| QCDT dt.img:', t[:4] == b'QCDT')
    if outdir:
        os.makedirs(outdir, exist_ok=True)
        for n, b in (('kernel', k), ('ramdisk', r), ('second', s), ('dt', t)):
            if b:
                open(os.path.join(outdir, n), 'wb').write(b)
    return dict(ks=ks, ka=ka, ra=ra, sa=sa, ta=ta, ps=ps)


def pack(a):
    ps = a.pagesize
    k = open(a.kernel, 'rb').read()
    r = open(a.ramdisk, 'rb').read() if a.ramdisk else b''
    base = a.base
    sha = hashlib.sha1()
    for b in (k, r, b''):
        sha.update(b); sha.update(struct.pack('<I', len(b)))
    cmd = a.cmdline.encode()
    hdr = struct.pack(HDR, b'ANDROID!', len(k), base + 0x8000, len(r), base + 0x01000000,
                      0, base + 0x00f00000, base + 0x100, ps, 0, 0, b'', cmd[:512], sha.digest().ljust(32, b'\0'),
                      cmd[512:])
    pad = lambda b: b + b'\0' * ((-len(b)) % ps)
    open(a.o, 'wb').write(pad(hdr) + pad(k) + pad(r))
    print('wrote', a.o, os.path.getsize(a.o))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    p = sp.add_parser('info'); p.add_argument('img')
    p = sp.add_parser('unpack'); p.add_argument('img'); p.add_argument('outdir')
    p = sp.add_parser('pack')
    p.add_argument('--kernel', required=True); p.add_argument('--ramdisk')
    p.add_argument('--cmdline', default='console=null')
    p.add_argument('--base', type=lambda x: int(x, 0), default=0x80000000)
    p.add_argument('--pagesize', type=int, default=2048)
    p.add_argument('-o', required=True)
    a = ap.parse_args()
    if a.cmd == 'info': info(a.img)
    elif a.cmd == 'unpack': info(a.img, a.outdir)
    else: pack(a)
