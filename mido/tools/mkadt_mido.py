#!/usr/bin/env python3
"""Build the Apple Device Tree for the MIDO xnu (Redmi Note 4, MSM8953).

Structure follows ios-sweet tools/mkadt.py (MIT); every address and number
here comes from the mido live FDT or was measured with the mido shim (see
PORTING_LOG.md). Usage:  mkadt_mido.py OUT.adt [--dump] [--product D211AP.adt]
"""
import hashlib, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))   # ios-sweet adt.py
import adt  # noqa: E402

# Kernel-managed RAM: DRAM is 0x40000000-0x100000000 (3 GB). Every fixed
# carve-out (modem, adsp, wcnss, splash, pstore ...) ends below 0xA0000000,
# and LK stages the ramdisk/payload at ~0x43600000. Take the top 1.5 GB.
PHYS_BASE = 0xa0000000
MEM_SIZE = 0x60000000
PANIC_LOG_SIZE = 0x100000          # carved off the top, exposed as /pram

SOC_BASE = 0x01000000              # /arm-io children are relative to this (must be != 0)
GICD, GICD_SIZE = 0x0b000000, 0x1000   # qcom,msm-qgic2
GICC, GICC_SIZE = 0x0b002000, 0x1000
VTIMER_INTID = 20                  # DT <GIC_PPI 4>; measured 29 IRQs / 300 ms
TIMEBASE = 19200000

FB, FB_SIZE = 0x90001000, 1080 * 4 * 1920      # shim switched RGB0 to 32 bpp
FB_W, FB_H, FB_STRIDE = 1080, 1920, 1080 * 4

# MSM8953: two clusters of four Cortex-A53, MPIDR Aff1:Aff0 (measured: PSCI CPU_ON 8/8)
CPU_MPIDRS = [0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103]
CPU_HZ = 2016000000                # DT clock-frequency 0x7829b800

MIDO_ECID = 0x4d49444f00000001     # 'MIDO' + 1, synthetic (not the phone's)
BOOT_MANIFEST_HASH = hashlib.sha384(b'mido boot manifest').digest()

# Userland identity: iPhone 8 Plus (iPhone10,5 / D211AP). Its physical panel is
# 1080x1920 like mido's (UI rendered at 1242x2208 @3x, downsampled). A11, same
# dyld cache code as the iPhone 8 scanned in PORTING_LOG.md.
MODEL, TARGET = 'iPhone10,5', 'D211AP'

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def product_props(path):
    """/product of the real D211AP tree (an Apple artifact, never committed)."""
    if not path or not os.path.exists(path):
        print('note: no D211AP device tree given, building without /product', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            # The panel is not P3. With "P3" backboardd asks for a deep-colour b3a8 framebuffer,
            # which the GL layer cannot attach (same fix as ios-sailfish's mkadt.py).
            return [(k, b'sRGB\0' if k in ('artwork-display-gamut', 'display-gamut') else v)
                    for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')


def build(seed=None, product_path=None):
    phys_base, mem_size = PHYS_BASE, MEM_SIZE
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('mido-shim')),
        ('system-firmware-version', cstr('mido-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        ('display-scale', u32(3)),
        ('unique-chip-id', struct.pack('<Q', MIDO_ECID)),
        ('chip-id', u32(0x8015)),                 # A11, as the iPhone 8 Plus userland expects
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    cpu_nodes = []
    for i, mpidr in enumerate(CPU_MPIDRS):
        cpu_nodes.append(node(f'cpu{i}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('arm,cortex-a53') + cstr('ARM,v8')),
            ('reg', u32(mpidr)),                  # MPIDR Aff1:Aff0, not the logical id
            ('cpu-id', u32(i)),
            ('cluster-id', u32(0)),               # one homogeneous SMP cluster for XNU
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(i)),
            ('l2-cache-id', u32(mpidr >> 8)),     # one L2 per physical cluster
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if i == 0 else 'stopped')),
            ('timebase-frequency', u32(TIMEBASE)),
            ('clock-frequency', u32(CPU_HZ)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(933000000)),
            ('peripheral-frequency', u32(TIMEBASE)),
            ('fixed-frequency', u32(TIMEBASE)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    # pexpert/arm/pe_fiq.c (MIDO GICv2): reg = <gicd off, size, gicc off, size> from SOC_BASE
    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v2')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(VTIMER_INTID)),
        ('reg', u64(GICD - SOC_BASE, GICD_SIZE, GICC - SOC_BASE, GICC_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1242)),          # iPhone 8 Plus logical canvas @3x
            ('canvas-height', u32(2208)),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # iPhone 8 Plus has a Home button: home-button-type absent (= 0).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('opposed-power-vol-buttons', u32(0)),
    ])

    pp = product_props(product_path)
    product = [node('product', [ph()] + pp)] if pp else []
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    # 'xiaomi,sweet' kept: ios-sweet's SweetPlatformExpert matches on it.
    return node('device-tree', [
        ph(),
        ('compatible', cstr(TARGET) + cstr(MODEL) + cstr('xiaomi,sweet') + cstr('xiaomi,mido') +
         cstr('qcom,msm8953') + cstr('AppleARM')),
        ('model', cstr(MODEL)),
        ('target-type', cstr('D211')),
        ('target-sub-type', cstr(TARGET)),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + product)


if __name__ == '__main__':
    out = sys.argv[1]
    prod = sys.argv[sys.argv.index('--product') + 1] if '--product' in sys.argv else None
    blob = adt.serialize(build(product_path=prod))
    open(out, 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{out}: {len(blob)} bytes', file=sys.stderr)
