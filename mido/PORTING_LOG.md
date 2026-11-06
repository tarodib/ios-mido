# iOS 16 → Redmi Note 4 (mido) — Porting Log

References: ios-sweet (Redmi Note 10 Pro, XNU xnu-8796 + iPhone X iOS 16.7.16 userland, shim boot),
hackiosx.dev porting guide / compatibility notes.

## 2026-11-06 — Session 1: recon + backups

### Host
- Windows 11, adb/fastboot (Minimal ADB) present, Python present.
- MISSING: WSL/Linux (needed for HFS+/APFS tools, mkbootimg, aarch64 gcc), git, gh CLI.
- XNU builds on GitHub macOS runners (needs a GitHub account + fork of ios-sweet).

### Device (read from OrangeFox, adb root)
| Item | Value | Implication |
|---|---|---|
| SoC | MSM8953 (Snapdragon 625), 8× Cortex-A53 r0p4 | ARMv8.0 — no LSE, RDM, FP16 |
| Page granules | A53 supports 4 KB / 64 KB only | **No 16 KB pages → need a 4 KB-page XNU build** (as UniXNU / Redmi Note 12 ports do) |
| RAM | ~2.9 GB (3 GB variant) | OK (Pixel 2016 port runs iOS 16 in 2 GB) |
| Interrupt controller | `qcom,msm-qgic2` @ 0x0B000000 (**GICv2**) | ios-sweet assumes GICv3 → new GICv2 driver needed |
| Timer | arch_timer, 19.2 MHz (PPI 20 via MPM) | standard |
| Storage | eMMC, `sdhci` @ 0x07824900 | ios-sweet is UFS-only → **new SDHCI-MSM eMMC driver** |
| UART | msm_serial_dm @ 0x078B0000 (ttyMSM0, 115200) | early console possible if test points are wired |
| Display | NT35596 Tianma FHD, **video mode**, 1080×1920, splash @ 0x90001000 | Reuse bootloader framebuffer; refreshes on its own (good) |
| Touch | FocalTech FT5435 @ i2c-3 (0x78B7000) addr 0x38 | new touch driver; power via RPM (not RPMh) |
| Bootloader | Qualcomm LK (aboot), unlocked (verifiedbootstate=orange) | boot.img v0/v1 + appended QCOM DTB, not ABL/UEFI → shim must be repackaged |
| userdata | mmcblk0p49, 23 GB, **19 GB in use** | iOS root (16 GB) goes here → **wipes Android data** |

Raw DTB: `hw/mido.dtb`.

### Backups (done, verified)
All firmware/calibration partitions + system pulled to `backup/stock/`
(persist, modemst1/2, fsg, fsc, modem, boot, recovery, aboot, sbl1, tz, rpm, dsp, splash, …).
SHA-256 of persist/modemst1/modemst2/fsg/fsc/boot/aboot matches on device and host
(`backup/stock/SHA256SUMS-device.txt`). Restore any of them with
`fastboot flash <name> backup/stock/<name>.img`.
NOT backed up: userdata (personal data — user to save it).

### Gap analysis vs ios-sweet
1. Boot chain: LK aboot → `fastboot boot shim.img` (v0 header + appended msm8953 DTB with matching msm-id/board-id) → shim → XNU.
2. XNU: rebuild xnu-8796 with 4 KB pages (PAGE_SHIFT 12) for Cortex-A53; check iOS userland for ARMv8.1+ instructions
   (the hackiosx guide says iPhone 8 firmware runs on ARMv8.0 without emulation in the Pixel 2016 port, so use iPhone 8 (iPhone10,x) firmware, not iPhone X).
3. New drivers: GICv2, SDHCI-MSM eMMC (bootloader leaves eMMC up → can start from warm state), FT5435 touch, RPM regulator for touch power.
4. ADT: generate from mido.dtb (memory map avoiding modem/TZ/qseecom reserved regions, 8 CPUs via PSCI, framebuffer).
5. Logging: reserved-RAM log buffer → pull from OrangeFox after reboot (as ios-sweet does), plus framebuffer text.

## 2026-11-06 — Session 1 (cont.): toolchain + M1 shim

### Host tooling (no WSL yet — needs admin)
- Installed git 2.56, gh 2.103, Python 3.12 (user scope).
- Arm GNU Toolchain 13.3.rel1 `aarch64-none-elf` (Windows, no admin) → `toolchain/`.
- `wsl --install` / `winget install Microsoft.WSL` both fail: **needs an elevated shell** (user action).
- ios-sweet cloned to `upstream/ios-sweet` @ 45263be (2026-10-23).
- Own tools: `tools/dtdump.py` (FDT reader), `tools/bootimg.py` (LK boot.img v0 info/unpack/pack).

### Boot-chain facts (stock boot.img)
- boot.img v0, page 2048, base 0x80000000, kernel 0x80008000, ramdisk 0x81000000, tags 0x80000100.
- Kernel = **Image.gz + appended DTB** (msm-id 0x125, board-id 0xb). No QCDT dt.img.
- `fastboot oem device-info`: unlocked, critical unlocked.

### More hardware addresses (from DT)
- GICv2: GICD 0x0B000000, GICC 0x0B002000. Arch timer PPIs 2/3/4/1 (sec-phys 18, ns-phys 19, virt 20, hyp 17).
- PS_HOLD 0x004AB000; IMEM restart_reason 0x0860065C; PSCI 1.0 via SMC; all CPUs `enable-method = psci`.
- eMMC sdhci-msm: 0x07824900 (+ core 0x07824000), IRQs SPI 0x7b / 0x8a.
- UART BLSP1 0x078AF000 (enabled), 0x078B0000 (disabled).
- Memory: 0x40000000–0xC0000000 (2 GB @1G + 2 GB @2G banks in DT; ~2.9 GB usable).
  Reserved: 0x84A00000+1E00000 other_ext, 0x86C00000+6A00000 modem, 0x8D600000+1100000 adsp,
  0x8E700000+700000 wcnss, 0x90001000+13FF000 splash, 0x9FF00000+100000 pstore.

### M1 test 1 — `shim/` (start.S + shim.c, built by `shim/build.py`)
- Paints 3 bands into splash fb (24bpp blue/red | white | green), waits 20 s,
  writes restart_reason=bootloader, PSCI SYSTEM_RESET, PS_HOLD fallback.
- `fastboot boot shim/out/shim-boot.img` → LK: OKAY. Phone then booted **Android**
  ~22 s later (uptime 58 s at t≈80 s).
- **Observation:** our code ran (20 s hold + reset timing). **Not honoured:** IMEM
  restart reason → LK on 8953 likely reads PMIC PON SOFT_RB_SPARE instead. TODO.
- **User observed:** top RED, middle WHITE, bottom GREEN, all solid, no stripes.
  → **Framebuffer = 24bpp, 1080×1920, stride 3240 bytes, byte order B,G,R** (byte 0 = blue)
  at 0x90001000; LK leaves the video-mode panel scanning it after `fastboot boot`.
- **M1 DONE** (code runs + visible output).

## 2026-11-06 — Session 2 (after host reboot)

### Host
- WSL2 installed by user (admin). Ubuntu-24.04 distro registered (`wsl --install -d Ubuntu-24.04 --web-download --no-launch`, no admin needed).
- apt: build-essential, gcc-aarch64-linux-gnu, hfsprogs, img2simg, mkbootimg, dtc, cmake, fuse3 dev libs. Checked by `tools/wsl-check.sh`.
- **Blocked:** `tools/wsl-setup-tools.sh` (build apfs-fuse from GitHub, fetch blacktop/ipsw release)
  was refused by the agent's safety classifier (downloads and runs third-party code). Needs user approval.

### M1.5 diagnostic shim (`shim/shim.c`, `shim/console.c`)
- Original 5×7 font text console, BGR888, 4× scale (45×60 chars).
- Probes: EL, MIDR/MPIDR, MMFR0 (16K granule), ISAR0 (LSE), PSCI version + CPU_ON ×7,
  GICv2 vtimer IRQ (INTID 20) count, SPMI PMIC-arb v2 PON channel + registers.
- Reboot to fastboot by itself:
  - Run 1: IMEM restart_reason + PON warm reset → still booted Android (~40 s). Shim ran to completion.
  - Run 2: + `PON_SOFT_RB_SPARE[7:2] = 2` → **BACK IN FASTBOOT after 35 s.** ✅
    → LK on mido uses the PON spare reboot register (USE_PON_REBOOT_REG); IMEM reason alone is ignored.
- Log retrieval:
  - pstore 0x9FF00000 unusable: Android/recovery ramoops wipes non-ramoops data.
  - Moved RAM log to 0x8EC00000 (inside no-map wcnss_fw_region).
  - **The backed-up OrangeFox (recovery.img, kernel 3.18.71) has no /dev/mem, kcore or kmem**
    (CONFIG_DEVMEM off; MODULES=y + MODVERSIONS). The OrangeFox the phone was originally in
    ran kernel 4.9.326 (zeelog), so the recovery partition doesn't hold the same build.
  - Plan: eMMC (SDHCI) write path in shim → log to `cache` partition (256 MB, unused) → `adb pull`.
    Until then: read results from screen / photo.
- Android on the phone is LineageOS (adb product `lineage_mido`), no root.

### M1.5 results (read off the screen by the user)
| Probe | Result | Consequence |
|---|---|---|
| 16K PAGES | **NO** | XNU must be built with **4 KB pages** (ARM_PGSHIFT 12), like UniXNU / Redmi Note 12 ports. ios-sweet's 16K kernel can't run here at all. |
| LSE ATOMICS | **NO** | Kernel built for ARMv8.0 (`-mcpu=cortex-a53`, no outline-atomics assumptions). Userland: use iPhone 8 firmware (reported ARMv8.0-clean on Pixel 2016) **and verify** by scanning dyld_shared_cache for CAS/LDADD/SWP etc.; fallback = undef-instruction emulation in XNU. |
| CPUS ONLINE | **8/8** | PSCI CPU_ON works for MPIDR 0x0–0x3, 0x100–0x103 → ios-sweet's SMP patch approach (0065/0078) carries over. |
| VTIMER IRQS / 300 ms | **29** (expect ~30), other 0 | GICv2 distributor/CPU interface + virtual timer PPI (INTID 20) work from NS-EL1. Needs a GICv2 interrupt controller driver in XNU (ios-sweet is GICv3-only). |

**M1.5 DONE.** Hardware basics needed by XNU (CPU, SMP, IRQ, timer) all confirmed.

### Tooling
- User approved one exact run of `tools/wsl-setup-tools.sh`: apfs-fuse 66b86bd (built) and ipsw 3.1.739 in `/root/tools/bin` (WSL).
  Each further use of these binaries needs the user's OK (agent classifier rule).

### GitHub
- User ran `gh auth login` → logged in as `tarodib`.
- `gh repo fork dipa-studio/ios-sweet --fork-name ios-mido` was **refused by the agent's safety
  classifier** (creates a public repo). Needs the user to fork it themselves or explicitly allow it.

### eMMC driver v0 (`shim/sdhc.c`) — PIO, single-block CMD17/CMD24
- Reference (Linux): cache = mmcblk0p25, LBA 7471104, 524288 sectors, unmounted. eMMC "RX1BMB", 61071360 sectors.
  GPT LBA0–33 saved to `hw/gpt-lba0-33.bin`; host parse agrees (entry 25).
- Sequence: re-enable HC mode (core+0x78) → CMD13 / CMD5 awake / CMD7 select with RCA 2 then 1 →
  read LBA0 (0xAA55), LBA1 ("EFI PART"), find "cache" in GPT → writes allowed ONLY to
  [7471104, 7471104+128) and only on an exact GPT match → write 64 KB RAM log, read back.
- Run 1: back in fastboot after 25 s, but **no log in cache** (cache still holds its original ext4,
  superblock at 0x400). GPT md5 on device == backup → nothing written anywhere. One of init/read/GPT
  checks failed and the guards refused. Need the on-screen EMMC lines to see which.

### GitHub fork
- User approved the exact fork command → **https://github.com/tarodib/ios-mido** (fork of dipa-studio/ios-sweet, branch main).

### eMMC driver, runs 2–4 (side channel: hold = 5 s + 4 s × code, decoded by `tools/runshim.ps1`)
- Photo of run 1 screen: `HC_MODE 0→1`, `PRES=01F80000`, `HCTL=00000030` (**bus power OFF**),
  `CLK=000F0003` (internal clk on, **SD clk off**), `HCTL2=00030000` (HS200 timing left selected).
- Run 2: + bus power via MSM PWRCTL handshake (core+0xDC/E0/E4/E8) → code 2 (power OK now; wake path
  CMD13/CMD5/CMD7 never reaches TRAN — HS200 state/DLL presumably lost in LK's mode switch).
- Run 3: full re-init (legacy timing, 1-bit, SDHCI divider ≈400 kHz, CMD0/CMD1…) → **code 10: CMD1 never ready**.
  Suspect the SDHCI internal divider is ignored on MSM (Linux uses ALWAYS_USE_BASE_CLOCK and sets
  the rate in GCC) and/or vendor HS400 clock routing (CORE_VENDOR_SPEC MCLK_SEL / SELECT_IN) still active.
- Next: program GCC SDCC1 RCG (0x01842004) to 400 kHz from XO, divider bypass, reset vendor clock
  routing + DLL, then 25 MHz from GPLL0 for data.

### eMMC runs 5–7 → **WORKING** ✅
- Run 5 (GCC SDCC1 RCG 0x01842004 → 400 kHz from XO, SDHCI divider bypass): GCC readback proves the
  offsets (before CFG=0x205 = GPLL4/3 ≈ 384 MHz HS400; after CFG=0x2017; CBCR=0x4FF1 running).
  Still CMD1 **command timeout** (ERRSTS=0001).
- Run 6 (+ PM8953 L5/L8 status via SPMI SID1 0x44/0x47, TLMM SDC1 pads 0x0110A000, Linux-style
  CORE_POWER SW_RST + HC_MODE_EN|FF_CLK_SW_RST_DIS + SDHCI RESET_ALL):
  L5 `ST=82 EN=80` (1.8 V vccq on), L8 `ST=82 EN=80` (2.9 V vcc on), pads already active (0x9FE4).
  Still timeout. Diagnostic line `VS=00000000` exposed the bug →
- **Root cause:** in 3.18 sdhci-msm (MCI present) `CORE_VENDOR_SPEC` (0x10C) and `CORE_DLL_CONFIG`
  (0x100) are offsets from the **SDHCI base 0x07824900**, not the core block. After the core reset the
  **IO pads were left in 3 V mode** while VCCQ is 1.8 V, so the controller never saw the card's response.
- Run 7: VENDOR_SPEC 0x358B1C → 0x18A1C (MCLK_SEL default, SELECT_IN off,
  IO_PAD_PWR_SWITCH_EN|IO_PAD_PWR_SWITCH = 1.8 V pads), DLL reset+PDN, HCTL2 1.8 V signaling
  (IO_LOW pwr request 0x04 acked) →
  `CMD1 OCR=C0FF8080` (ready, sector mode), CMD2/3(RCA 2)/7 → TRAN at 25 MHz (GPLL0/16·½, legacy, 1-bit),
  LBA0 AA55, LBA1 GPT, cache #25 matches Linux, **64 KB log written + verified + auto-pulled**.
- Working bring-up sequence (= what the XNU eMMC driver must do after LK):
  1. CORE_POWER(core+0) SW_RST; HC_MODE(core+0x78)=HC_MODE_EN|FF_CLK_SW_RST_DIS; SDHCI RESET_ALL
  2. Bus power 0x0D → ack PWRCTL (core+0xDC/E4/E8) BUS_ON
  3. VENDOR_SPEC(hc+0x10C): MCLK_SEL=2, clear SELECT_IN, set IO_PAD_PWR_SWITCH(_EN); DLL(hc+0x100) RST|PDN
  4. HCTL2 1.8 V signaling → ack IO_LOW; legacy timing, 1-bit
  5. GCC SDCC1 RCG: XO/12·¼ = 400 kHz; SDHCI divider 0; CMD0, CMD1 (0x40FF8080) until ready, CMD2, CMD3, CMD7
  6. GCC SDCC1 RCG: GPLL0/16·½ = 25 MHz. (Later: 8-bit + HS200/HS400 with tuning for speed.)
- **Test loop is now fully automatic:** `tools/runshim.ps1` builds, boots, decodes the timing code,
  boots OrangeFox from RAM and pulls the log from `cache` (`shim/out/shimlog.txt`). No photos needed.

### Display: LK pipe state + switch to 32 bpp
- MDSS/MDP5 probe (MDP HWVER 0x10100000): LK scans out **RGB0** (0x01A15000): SRC0_ADDR 0x90001000,
  SRC_FORMAT 0x0002243F (3 B/px, 3 comps, tight), UNPACK 0x00020001 (bytes B,G,R), YSTRIDE 0xCA8,
  SIZE 0x0780_0438. CTL0 (0x01A02000) LM0=0x200 (RGB0 staged), TOP=0x1F20. VIG0/RGB1/DMA0 unused.
- `fb_to_32bpp()`: SRC_FORMAT 0x000236FF (4 B/px, 4 comps, A 8-bit, alpha off), UNPACK 0x03020001
  (B,G,R,X), YSTRIDE 4320, CTL0_FLUSH bit3 (RGB0). Readback FMT=0x236FF, STRIDE=0x10E0, FLUSH→0
  (consumed at vsync). 32 bpp frame = 8.3 MB inside splash carve-out (20 MB). Console draws 32-bit pixels.
  **User confirmed: 32 bpp console looks correct** (navy background, crisp white text, right colours).
- Shim hold on success is now 30 s (HOLD_BASE) so the screen can be read; runshim.ps1 subtracts it.
  → XNU boot_args Video: base 0x90001000, rowBytes 4320, 1080×1920, depth 32.

### Loader step 1: payload discovery (`shim/fdt.c`, `probe_payload()`)
- `fdt_chosen_u64()` reads `/chosen` from the DTB LK passes in x0 (0x43400000).
- Test with a 4 MB header-only dummy payload (`build.py <payload>` puts it in the ramdisk):
  **LK placed the ramdisk at 0x43600000–0x43A00000** (just above the DTB, NOT the header's 0x81000000).
  Magic SWEETXNU + header fields read back exactly. No jump yet.
- XNU RAM window chosen: **0xA0000000, 1.5 GB** (above every carve-out incl. pstore 0x9FF00000;
  DRAM ends at 0x100000000). Ramdisk/payload staging area stays below 0x84A00000.

### Loader step 2: full XNU loader (`shim/loader.c`, from ios-sweet loader.c, MIT)
- Copies payload segments to phys_base + (vmaddr − virt_base), ADT then boot_args (16K-aligned)
  behind the kernel, boot_args.Video = 0x90001000 / 4320 / 1080×1920 / depth 32, jumps MMU-off with
  x0 = boot_args PA. Only runs when the 32 bpp switch succeeded.
- Refuses (returns a reason, shim resets normally) if: no /chosen initrd, no SWEETXNU magic, bad
  header, **0 segments or 0-byte ADT**, RAM window ≠ 0xA0000000.., ramdisk overlaps kernel RAM,
  any segment outside the window. Log is flushed to `cache` before the attempt and again after a
  refusal.
- Test with the 4 MB header-only payload: `NOT BOOTED: PAYLOAD HAS NO KERNEL (HEADER-ONLY TEST)`,
  back in fastboot at 37 s, logged. runshim.ps1 timing decode re-centred on 6.5 s overhead.

### XNU CI
- `macos-14` runner retired → baseline run 42146912294 failed at job start.
- Workflow on branch `mido` takes a `runner` input (default macos-15) and lists installed Xcodes
  (commit 4715674; push needed `gh auth refresh -h github.com -s workflow`).
- Baseline SWEET build on macos-15: run 42147239678 (in progress). Goal: buildtree.tgz (fully
  patched xnu-8796 tree) to write the MIDO patches (4K pages, GICv2, LSE emu, eMMC) as real diffs.
  Known risk: Xcode 16 iig emits IORPC.kernelContent, missing in xnu-8796 → compat patch if needed.

### Baseline XNU build + MIDO patch
- Run 42147239678 (macos-15, unmodified SWEET): **success**. Artifacts in `xnu/kernel-baseline/`
  (kernel.development.sweet 17 MB) and `xnu/buildtree-baseline/buildtree.tgz` (patched source, 23 MB).
  Xcode 16 iig did NOT break the build (no IORPC compat patch needed so far).
- Survey of the patched tree (`tools/wsl-xnu-survey.sh`):
  - `__ARM_16K_PG__`: 47 sites in ~10 files; proc_reg.h has ARM_PGSHIFT 12 path, pmap has ARM_4K_TT_* → 4K is a supported mode.
  - GICv3 use is confined to pe_fiq.c (init), sleh.c:1864-1891 (IAR/EOIR), SweetCPU.cpp:226 (SGI1R).
  - start.s/sweet_early.c/arm_vm_init.c hard-code sweet's fb 0x9c000000 / 2400 rows / one 32 MB L2 block.
- `mido/xnu/mido_xnu.py` (exact-string, count-checked edits) + `mido/xnu/mkpatch.sh` →
  `xnu/patches/0100-mido-board.patch` (9 files, +248/-6), `git apply --check` OK on the baseline.
  MIDO switch in SWEET.h: no 16K pages, HAS_GIC_V2 (new pe_fiq.c path + mido_gic_ack/eoi/send_sgi),
  vtimer 20, -march=armv8-a+crc+crypto, fb 0x90001000 (stage-0 band, 4×2 MB bootstrap blocks,
  early console, non-fatal 4-block KVA alias).
- Commit 031a768 on `mido`; first MIDO kernel build: run 42149040200 → **failed, one error**:
  `pmap.c:10988 static_assert((_COMM_PAGE64_BASE_ADDRESS & ~ARM_TT_L1_OFFMASK) >= MACH_VM_MAX_ADDRESS)`.
  With 4K pages the commpage must be nested above all user VA at L1 (1 GB) granularity; the
  build is a macOS-platform kernel (XNU_PLATFORM_MacOSX → MACH_VM_MAX 0x7FFFFE000000). The 16K
  path explicitly exempts macOS (commpage inside a VM-reserved region). Everything else compiled
  (make -k reported nothing else), incl. the GICv2 pe_fiq.c path, sleh.c, SweetCPU.cpp, start.s.
  Log saved: `xnu/ci-42149040200.log`.
- Fix (commit 50bc312): `-DMIDO_4K_VM` in MACHINE_FLAGS selects the **iOS** value
  MACH_VM_MAX_ADDRESS = 0xFC0000000 (= 0xFFFFFC000 & ~(1 GB−1)); the userland is iOS anyway.
  Build #2: run 42150230384.
- `mido/tools/mkadt_mido.py` → `xnu/adt/mido.adt` (9.9 KB): RAM 0xA0000000 + 1.5 GB (− 1 MB /pram
  panic log), GICv2 reg + vtimer 20, 8× A53 (reg = MPIDR), sweet-fbcon 0x90001000 1080×1920×4320,
  identity **iPhone 8 Plus (D211AP, iPhone10,5)**: same 1080×1920 physical panel, A11 userland.
  /product (from Apple's D211AP DeviceTree) not included yet — only needed for userland.

### FIRST XNU BOOT ATTEMPT (build #2)
- Run 42150230384: **MIDO kernel compiled** (4K pages, GICv2, ARMv8.0, iOS VA layout).
  `xnu/kernel-mido-2/kernel.development.sweet`, plain arm64 (cputype 0x100000c), 0 chained fixups.
- `repo/tools/mkpayload.py kernel mido.adt payload-mido-2.bin --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -v"`:
  16.2 MB, virt_base 0xfffffe0006000000 (→ PA 0xA0000000), entry 0xfffffe0007427488,
  kernel_end 0xfffffe0008464000 (→ PA 0xA2464000), 10 segments.
- shim-boot.img 16.5 MB (fastboot max-download-size 0x20000000 = 512 MB).
- `fastboot boot` → phone did **not** return to fastboot within 120 s → shim jumped to XNU.
- **User photo:** shim output intact up to `BOOT ARGS PA=A2468000`, `TOP OF KERNEL=A246C000`,
  `JUMP=A1427488`, `JUMPING TO XNU`, plus **XNU's stage-0 white band (64×32 px) at the bottom-left,
  row 1888** = the MIDO start.s patch. → **XNU's first instructions run on the phone** (entry,
  physical placement and fb address correct). No `SWEET EARLY` stage marks after it.
- **Root cause:** with 4K pages `T1SZ_BOOT = 25` → TTBR1 covers only the top 512 GB
  (0xffffff80_0000_0000+). MakeInc.def forces `ARM_LARGE_MEMORY` for every MacOSX-platform build
  → kernel linked at 0xfffffe00_0700_4000 (−2 TB). MMU enable succeeds (executing V=P), the return
  to `arm_init` at its KVA cannot translate, VBAR (also KVA) neither → silent hang.
- **Fix (commit 7d3090e):** MakeInc.def keeps the iOS layout for MACHINE_CONFIG=SWEET (link
  0xfffffff0_0700_4000, VM_MIN_KERNEL 0xffffffe0_0000_0000). Build #3: run 42152446197.
- Prepared (uncommitted in repo/, push only if build #3 stops in start.s): `MIDO_MARK n, color`
  in start.s — 48×32 squares right of the stage-0 band: 1 red (low vectors), 2 orange (bootstrap
  tables built), 3 yellow (TCR/TTBR/MAIR), 4 green (about to enable MMU), 5 cyan (MMU on),
  6 blue (ret to arm_init). x0–x3 parked in `mido_mark_save[4]` (sweet_early.c), x16/x17 scratch.
  Anchors limited to start_first_cpu (start_cpu duplicates caught by the count check).
  Patch with marks: +291/−7, `git apply --check` OK.
- User forced the phone back to fastboot (Power + Vol-Down) after the first attempt.

### XNU boot #2 (build #3, run 42152446197, link 0xfffffff007004000)
- payload: virt_base 0xfffffff006000000, entry 0xfffffff00740b488, kernel_end 0xfffffff008448000.
- **User report: `SWEET EARLY` 1, 2, 3, 4 on screen, then
  `panic: undefined kernel instruction: pc=0xfffffff0072b1c18 instr=b8280028 @sleh.c:951`.**
  → start.s, MMU enable, return to arm_init (C) and the early console/panic path all work.
- `mido/tools/kscan.py` (Mach-O symbol table + LSE scan): pc = `_lck_grp_reference+0x8`;
  b8280028 = `ldadd w8, w8, [x1]`. Kernel code has **843 LSE instructions in 517 functions**
  (pmap_ro_zone_atomic_op_internal, pset_update_processor_state, os_ref_release_internal, ...).
- Root cause: `-march=armv8-a+crc+crypto` IS on all 1102 compile lines, but `-arch arm64
  -mmacosx-version-min=15.0` makes Apple clang default the *target CPU* to an Apple core, whose
  features (LSE, RCpc, FP16, ...) `-march` does not remove.
- Fix (commit a9a15a9): `-mcpu=cortex-a53+crc+crypto`. Also includes the start.s MIDO_MARK 1–6
  squares. Build #4: run 42153729707. Check after build: kscan.py must report 0 LSE.
- Build #4 **failed to compile** — the real ARMv8.0 target now rejects v8.1 features (good):
  - `Invalid register name "pan"` (copyio.c, pcb.c, platform_tests.c): PAN is ARMv8.1, A53 lacks it →
    `__ARM_PAN_AVAILABLE__` only when !MIDO.
  - `commpage_asm.s:276/326: instruction requires: lse`: the PFZ `pfz_trylock_and_enqueue/dequeue`
    routines (commpage text, mapped into every process) use `casa`. MIDO: LDAXR/STXR loop, expected
    0 / new 1, w12/w13 scratch (x9–x15 allowed), old value in w10, clrex on the held path.
  - Patch +326/−9 (11 files). Commit 0fe06f7, build #5: run 42154918199.
- Build #5 **failed only in commpage_asm.s** (PAN fix worked; everything else compiled): the `#else`
  `casa` branch was assembled because commpage_asm.s does not include SWEET.h → `MIDO` undefined.
  Fix (commit 9525aee): MACHINE_FLAGS adds `-DMIDO_CPU_ARMV8_0`; the PFZ replacement tests that.
  Lesson: MIDO-only code in files that don't include the board header must use a command-line flag.
  Build #6: run 42156033508 → **compiled**.
- kscan.py on build #6: LSE **843 → 12**: 11 in `firehose_*` (os_log kernel side) + 1 false
  positive (`K256` = SHA-256 constants inside __TEXT_EXEC). Not booted (firehose runs from early boot).
- firehose comes from `libfirehose_kernel`, built by xcodebuild from libdispatch (not xnu make), so it
  never saw MACHINE_FLAGS; macOS SDK default arm64 CPU = Apple core → LSE.
- Fix (commit d21f54f): `mido/ci/patch_dxb.py` (exact, count-checked) replaces the workflow sed:
  libfirehose_kernel → `ARCHS="arm64" VALID_ARCHS="arm64" OTHER_CFLAGS='$(inherited) -mcpu=cortex-a53+crc+crypto'`.
  Build #7: run 42157054345. Boot only if kscan.py shows 0 real LSE.
- Build #7 compiled; **kscan.py: only `K256` (data) left → 0 real LSE in kernel code.**
  payload: virt_base 0xfffffff006000000, entry 0xfffffff007424488, kernel_end 0xfffffff0084a4000.
- XNU boot #3 (build #7): shim jumped (no return to fastboot in 120 s). Rerun gave the same. **First real
  XNU console output**: "Darwin Kernel Version 22.6.0 ... xnu-8796.141.3", pmap_startup done
  (95402 free pages, 2758 wired), VM bootstrap OK, then
  `panic: kmem_range_init: vm_map_locate_space failing for claim zones @vm_kern.c:3645`, followed by
  nested panics. The 'iBoot didn't extract panic log' line is expected because there's no iBoot.
  - Cause: SWEET is built as an `XNU_PLATFORM_MacOSX` kernel, so zalloc asks for a 128 GB zone map
    (ZONE_MAP_VA_SIZE). Without ARM_LARGE_MEMORY (not possible with the 4K/39-bit VA) the kernel map is
    0xffffffe000000000..0xfffffffbffffffff = 112 GB, so the claim can't fit. ios-sweet fits it in the
    2 TB large-memory layout.
  - Fix (mido_xnu.py): under MIDO_4K_VM use the iOS sizes (ZONE_MAP_VA_SIZE 24 GB, ZONE_MAP_MAX 8 GB).
    This also cuts zone metadata VA from 512 MB to 96 MB at 4K pages. The other claims (ptr, data,
    sprayqtn) are sized from the remaining free space. → build #8.
- XNU boot #4 (build #8, run 42182690814, commit 5e4392e): kscan clean (only the K256 data match).
  Shim jumped. Result: **same panic** (`claim zones @vm_kern.c:3645`), so the 128 GB size wasn't
  the only problem.
  - Kernel map layout (4K, no KTRR, no large memory): it is created over VM_MIN_KERNEL_ADDRESS
    0xffffffe000000000 .. VM_MAX 0xfffffffbffffffff. Reserved inside it: pmap_virtual_region 0
    (gVirtBase..virtual_space_start), VREGION1 (top), low globals. That leaves a ~64 GB hole
    below 0xfffffff000000000 and a ~46 GB hole above the kernel. kmem_add_extra_claims sizes
    the ptr/sprayqtn/data claims so that all claims fill the largest hole exactly. The
    compressor, bsd and ipc claims are small (scaled to 1.5 GB of RAM), so no obvious overflow.
  - Can't settle it from the source, so build #9 adds `mido_kmem_claims_dump()`. On failure it
    prints the map bounds, the cursor, the largest free hole, every claim and size (the failing
    one marked with *), and every hole ≥ 16 MB. It then retries the claim from the bottom of the
    map before panicking.
- XNU boot #5 (build #9, run 42184628559, commit 4ab8897). The dump worked:
  - kmap fffffe0000000000-fffffffbffffffff (VM_MIN is -2 TB in this config, everything below the
    kernel is reserved). Holes: fffffff002590000-fffffff006000000 (58M) and
    **fffffff09e000000-fffffffbffdfc000 (46621M)**, which is the largest.
  - Claims: zones 24576M, **compressor 24562M**, gIOKitPageableFixed0-3 96M each, ipc_kernel_map
    64M, mb 64M, bsd_pageable 34M, zone_meta 28M, io_submap 8M, ipc_kernel_copy_map 8M,
    bufferhdr 1M. That's over 46621M, so `largest_free_size -= total_claims` underflowed:
    kmem_ptr_range 134192379M, kmem_data_range 17592051823687M. compressor failed first; the retry
    placed it, then kmem_ptr_range panicked (@vm_kern.c:3690).
  - Cause: the macOS compressor pool is 16 x max_mem (24 GB for 1.5 GB of RAM).
  - Fix: under MIDO_4K_VM cap the pool at 4 x max_mem (6 GB) unless the vm_compression_limit
    boot-arg is set. Fixed claims are about 31 GB, leaving about 15 GB for the ptr/sprayqtn/data
    ranges. → build #10.

### Userland ISA check — iPhone 8 (iPhone10,4) iOS 16.7.16 (20H392) dyld cache
- Fetched only the dyld cache from Apple (`tools/fetch-dyld.sh`, ipsw remote extract) → WSL `~/fw/iPhone10,4/`.
- `tools/isa-scan.py`: v1 scanned whole executable mappings → ~190k "LSE" hits, almost all
  **false positives** (__TEXT constants/strings). v2 scans only S_ATTR_PURE_INSTRUCTIONS sections of
  all 2705 images (1476 MB code). Report: `hw/isa-scan-iPhone10,4-20H392.txt`.
- Real (objdump-verified) ARMv8.1+ code:
  - **LSE atomics ~860 sites**: libdispatch (629), libsystem_pthread (82), libsystem_platform (72),
    libsystem_malloc (20), ActionKit, libsystem_m, libcorecrypto, libMobileGestalt, **dyld (1)**, …
  - **FP16 (fmul v.8h etc.)**: libBNNS (1290) + 1 site in ~24 media/store frameworks.
  - RDM / AudioCodecs-CASP hits decode as undefined → false positives. Dot product: 0. LDAPR: 1 (ActionKit).
- Disassembly around libdispatch hits (`tools/dsc-extract.py`): **inline, unguarded** `ldadd`/`cas`
  (no commpage feature check, no LL/SC fallback) → will raise undefined-instruction on Cortex-A53.
- **Decision: XNU needs an EL0 undefined-instruction emulator** for LSE (LDADD/LDCLR/LDEOR/LDSET/
  LD{S,U}{MAX,MIN}/SWP/CAS/CASP, all size/acquire/release variants) using LDXR/STXR on the user
  address with copyio-style fault recovery. FP16 emulation later (libBNNS/ML only, rare).
  Alternative for performance later: patch the cache to branch to LL/SC thunks.
  The hackiosx claim that iPhone 8 firmware is ARMv8.0-clean does NOT hold for 16.7.16.

## HANDOFF (current plan)
Done: M1 (code runs), M1.5 (CPU/SMP/GIC/timer/PMIC facts), eMMC read+write in shim, automatic log loop,
userland ISA analysis, GitHub fork tarodib/ios-mido.
Next, in order:
1. **Shim → XNU loader**: port ios-sweet `shim/loader.c` + `tools/mkpayload.py` to LK boot.img v0
   (payload in ramdisk @0x81000000), keep console/log/eMMC.
2. **XNU (fork tarodib/ios-mido, CI on macOS runners)**:
   a. 4 KB pages (A53 has no 16K granule) — check UniXNU / Redmi Note 12 port for the pmap changes.
   b. GICv2 interrupt controller driver (ios-sweet is GICv3-only), vtimer INTID 20.
   c. EL0 undefined-instruction emulator for LSE atomics (+ FP16 later).
   d. eMMC root device driver using the sequence above (replaces UFS patches 0028/0030/0034/0069).
   e. mido ADT via tools/mkadt.py from hw/mido.dtb; framebuffer 1080×1920 BGR888 @0x90001000.
3. iOS root image from iPhone 8 (iPhone10,4) 16.7.16 → userdata (needs full IPSW, ~6 GB, + mkiosroot.sh).

## HANDOFF — where to pick up (older)
- Next: fix self-reboot-to-fastboot (try PMIC PON SOFT_RB_SPARE via SPMI, as Linux
  `qpnp-power-on` does on 8953), then add an on-screen text console (port ios-sweet
  `shim/console.c` to BGR888) and a pstore-RAM log at 0x9FF00000.
- Then M2 prep: GICv2 + timer IRQ test in the shim, PSCI CPU_ON for cpus 1–7,
  check ID_AA64MMFR0 TGran16 (expect 0 = no 16K) to confirm 4K-page XNU is required.
- Blocked on user: WSL (`wsl --install -d Ubuntu-24.04` in admin PowerShell + reboot),
  later `gh auth login`.

### Bring-up milestones
- [ ] M1 shim runs: screen goes a solid colour via `fastboot boot`
- [ ] M2 XNU prints to framebuffer, reaches "waiting for root device"
- [ ] M3 eMMC driver mounts HFS+ root, launchd starts, Apple logo
- [ ] M4 lock screen (MobileGestalt identity, daemons disabled)
- [ ] M5 touch + buttons
