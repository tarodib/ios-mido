# ios-mido: XNU bring-up on the Redmi Note 4 (mido, Snapdragon 625)

Experimental port of the ios-sweet approach (Apple's open-source XNU + iOS 16 userland)
to the Xiaomi Redmi Note 4 (`mido`, MSM8953, 8× Cortex-A53, eMMC, Qualcomm LK bootloader).
Research project, not a usable phone OS. Nothing from Apple is stored in this repository.

The full engineering log, with every test result, is in [`PORTING_LOG.md`](PORTING_LOG.md).

## Status
| Milestone | State |
|---|---|
| Boot own code via `fastboot boot` (LK, boot.img v0 + appended DTB) | done |
| Framebuffer console (1080×1920 BGR888 @ 0x90001000) | done |
| Self-reboot to fastboot (PMIC PON SOFT_RB_SPARE + warm reset) | done |
| CPU facts: ARMv8.0, no 16K granule, no LSE; PSCI CPU_ON 8/8 | done |
| GICv2 + virtual timer IRQ (INTID 20) | done |
| eMMC (SDHCI-MSM) re-init, read, guarded write | done |
| Automatic test loop (log → `cache` partition → `adb pull`) | done |
| Userland ISA analysis (iPhone 8 iOS 16.7.16): LSE/FP16 need emulation | done |
| XNU: 4K pages, GICv2, LSE emulation, eMMC root, mido ADT | next |

## Layout
- `shim/` — bare-metal boot shim (LK → shim → XNU later). `python shim/build.py` builds
  `shim/out/shim-boot.img` with the Arm GNU `aarch64-none-elf` toolchain in `../toolchain/`.
- `tools/` — boot.img packer, DTB reader, test loop (`runshim.ps1`, `shimlog.ps1`),
  dyld-cache ISA scanner and WSL helpers.

The scripts expect a local working directory with `backup/`, `hw/` and `toolchain/`
next to `shim/` and `tools/` (see the log). Those hold device dumps and are never committed.
