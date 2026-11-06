#!/usr/bin/env python3
"""Build the mido shim into an LK-bootable boot.img:
   Image (padded to image_size) -> gzip -> + stock mido DTB appended -> boot.img v0.
Usage: build.py [ramdisk]"""
import glob, gzip, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TC = glob.glob(os.path.join(ROOT, 'toolchain', '*', 'bin'))[0]
CC = os.path.join(TC, 'aarch64-none-elf-gcc.exe')
OBJCOPY = os.path.join(TC, 'aarch64-none-elf-objcopy.exe')
NM = os.path.join(TC, 'aarch64-none-elf-nm.exe')
READELF = os.path.join(TC, 'aarch64-none-elf-readelf.exe')
CFLAGS = ['-O2', '-ffreestanding', '-nostdlib', '-fno-pic', '-mgeneral-regs-only',
          '-mstrict-align', '-fno-stack-protector', '-mcpu=cortex-a53', '-fno-builtin',
          '-fno-tree-loop-distribute-patterns', '-Wall', '-Wextra']
SRCS = ['start.S', 'shim.c', 'console.c', 'sdhc.c', 'spmi.c', 'fdt.c']
OUT = os.path.join(HERE, 'out')


def run(*a):
    subprocess.run(a, check=True)


def stock_dtb():
    k = open(os.path.join(ROOT, 'hw', 'stock_boot', 'kernel'), 'rb').read()
    off = k.find(b'\xd0\x0d\xfe\xed')
    size = struct.unpack_from('>I', k, off + 4)[0]
    return k[off:off + size]


def main():
    os.makedirs(OUT, exist_ok=True)
    objs = []
    for s in SRCS:
        o = os.path.join(OUT, os.path.splitext(s)[0] + '.o')
        run(CC, *CFLAGS, '-c', os.path.join(HERE, s), '-o', o)
        objs.append(o)
    # Position independence: LK chooses the load address; no ABS64 relocs in C objects.
    rel = subprocess.run([READELF, '-r', *objs[1:]], capture_output=True, text=True).stdout
    if 'ABS64' in rel:
        sys.exit('error: absolute relocations in C objects')
    elf = os.path.join(OUT, 'shim.elf')
    run(CC, *CFLAGS, '-static', '-T', os.path.join(HERE, 'shim.ld'), '-Wl,--build-id=none',
        '-Wl,--no-warn-rwx-segments', '-Wl,-N', *objs, '-o', elf)
    img = os.path.join(OUT, 'Image')
    run(OBJCOPY, '-O', 'binary', elf, img)
    end = int(re.search(r'^([0-9a-f]+) \w _end$', subprocess.run([NM, elf], capture_output=True, text=True).stdout, re.M).group(1), 16)
    data = open(img, 'rb').read().ljust(end, b'\0')
    dtb = stock_dtb()
    kernel = gzip.compress(data, 9) + dtb
    kpath = os.path.join(OUT, 'Image.gz-dtb')
    open(kpath, 'wb').write(kernel)
    rd = sys.argv[1] if len(sys.argv) > 1 else None
    if not rd:
        rd = os.path.join(OUT, 'empty.cpio.gz')   # LK dislikes a zero-size ramdisk
        trailer = b'070701' + b'0' * 8 * 11 + b'0000000B' + b'0' * 8 + b'TRAILER!!!\0'
        open(rd, 'wb').write(gzip.compress(trailer.ljust((len(trailer) + 511) // 512 * 512, b'\0')))
    run(sys.executable, '-I', os.path.join(ROOT, 'tools', 'bootimg.py'), 'pack', '--kernel', kpath,
        '--ramdisk', rd, '-o', os.path.join(OUT, 'shim-boot.img'))
    print('Image %d bytes (image_size %#x), DTB %d bytes' % (os.path.getsize(img), end, len(dtb)))


if __name__ == '__main__':
    main()
