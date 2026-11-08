#!/usr/bin/env python3
"""Print a file's on-disk extents with FIBMAP (filefrag gives no table on hfsplus)."""
import fcntl, os, struct, sys
FIBMAP = 1
fd = os.open(sys.argv[1], os.O_RDONLY)
nb = os.fstat(fd).st_size // 4096
start = prev = None
n = 0
for i in range(nb):
    b = struct.unpack('i', fcntl.ioctl(fd, FIBMAP, struct.pack('i', i)))[0]
    if prev is None or b != prev + 1:
        if prev is not None:
            print(n, start, prev - start + 1)
        n, start = i, b
    prev = b
print(n, start, prev - start + 1)
