#!/usr/bin/env python3
"""Extract raw bytes from a split dyld cache by virtual address.
Usage: dsc-extract.py <cache dir> <addr> <before> <after> <out.bin>"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib import import_module
Cache = import_module('isa-scan').Cache

d, addr, before, after, out = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), int(sys.argv[4], 0), sys.argv[5]
c = Cache(d)
open(out, 'wb').write(c.read(addr - before, before + after))
print('extracted %#x..%#x' % (addr - before, addr + after))
