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
- XNU boot #6 (build #10, run 42185899233, commit 2d158d1): **VM, zalloc and IOKit up.** The claim
  table prints sanely: kmem_data_range 10G, kmem_ptr_range 3939M, kmem_ptr_range_meta 752K,
  kmem_large_ranges[4] 655M and [5] 9G. vm_compressor_mode 4. The zone_init maps are VM 3684M,
  RO 1228M, GEN0-3 3684M each, DATA 4912M, metadata 24M, bits 4496K. Then, in order:
  - "VM bootstrap done: 85 maps".
  - firehose, oslog, "iBoot version: mido-shim".
  - AMFI built-in, vnguard, "trust caches unavailable".
  - "SWEET: starting device-tree platform expert".
  - AKS, WDT, IOSurfaceRoot, HID buttons and touch, static battery fallback.
  - `SWEET FB: AppleCLCD 1080x1920 stride 4320 at 0x90001000, default surface 1, presented as
    1125x2436`. The 1125x2436 comes from ios-sweet patch 0061 (iPhone X size), to revisit.
  - Nothing after that. With no `rd=` boot-arg, IOFindBSDRoot waits on IOKit for a root disk
    and none exists yet. This should be **M2: waiting for root device** (to confirm with
    "Still waiting for root device" on screen).
  - Next: an eMMC root driver for XNU (bsd/dev/mido_emmc.c, modelled on ios-sweet's sweet_ufs.c and
    the shim's sdhc.c), selected with `rd=emmcroot0`.
- Build #11 adds **userland prerequisites**, both MIDO-only in mido_xnu.py. MakeInc gets a new
  `-DMIDO_BOARD` flag, since sweet_ufs.c doesn't include SWEET.h.
  1. **eMMC transport under ios-sweet's sweet_ufs.c.** This is not a separate driver: everything
     above the transport stays, i.e. `rd=ufsroot0`, the `ufsrw=1` guard, the partition-bounds
     check in ufs_write_blocks, and the 4 MiB console log ring at the end of `userdata`
     (sweet-log-pull.sh works unchanged). Below it, SDHCI PIO goes through the buffer data port
     on the card the shim left selected (RCA 2, transfer state, 25 MHz, 1-bit):
     - CMD17/18 and CMD24/25 with auto-CMD12, 256 sectors per command, and a CMD13 ready poll
       after writes.
     - Sync is CMD6 FLUSH_CACHE; errors are logged once and ignored.
     - The GPT is read in 512-byte sectors, and the partition must be 4 KiB aligned. The
       driver's 4 KiB blocks are 8 sectors.
     - The SD730-only sweet_keys/touch/usb start-up is skipped.
  2. **EL0 LSE emulator** in sleh.c handle_uncategorized, which runs right after the instruction
     is copied in.
     - It decodes LD{ADD,CLR,EOR,SET,SMAX,SMIN,UMAX,UMIN} and SWP (mask 0x3F200C00 =
       0x38200000, excluding LDAPR), CAS (0x3FA07C00 = 0x08A07C00) and CASP (0xBFA07C00 =
       0x08207C00), in all sizes and A/L variants. Rt/Rs 31 = XZR, Rn 31 = SP, TBI cleared.
     - Unaligned access, or an address outside the map, falls back to SIGILL.
     - With interrupts off (cpus=1, so nothing else can run), it checks `AT S1E0W` and does
       the RMW directly at EL1 (no PAN on MIDO), with dmb ish around it. If the check fails
       it re-enables interrupts, runs arm_fast_fault and then vm_fault(RW), and retries up to
       8 times.
     - It prints "MIDO LSE: emulating ..." once.
     - **SMP limitation:** this isn't atomic against other cores, so userland must stay on
       cpus=1 until there is an IPI-based stop-the-world or a per-core scheme.
- **Root image (in progress):**
  - Downloaded iPhone_5.5_P3_16.7.16_20H392_Restore.ipsw (iPhone10,5, 6.2 GB) to WSL ~/fw/ipsw-10,5
    with `ipsw download`. BuildManifest (d211ap, Customer Erase): OS 087-86694-044.dmg,
    Cryptex1,SystemOS 087-86588-047.dmg, Cryptex1,AppOS 087-86715-047.dmg.
  - DeviceTree.d211ap.im4p was extracted to xnu/adt/d211ap.adt (not in git).
    `mkadt_mido.py ..\xnu\adt\mido.adt --product ..\xnu\adt\d211ap.adt` now gives a 12504-byte ADT
    with the real /product node.
  - tools/wsl-mkroot.sh: mkiosroot.sh had CRLF from the Windows checkout, so it runs from an
    LF copy. mkfs.hfsplus worked (16 GB journaled volume), but the **mount fails: the WSL2 kernel
    6.18.54.1 has CONFIG_HFSPLUS_FS unset** (no module either).
  - Options:
    - (a) build hfsplus.ko out-of-tree from microsoft/WSL2-Linux-Kernel tag
      linux-msft-wsl-6.18.54.1 and insmod it (tools/wsl-hfsplus-ko.sh; needs user approval);
    - (b) do the copy on the phone from OrangeFox if its kernel has hfsplus (tar over adb);
    - (c) a custom WSL kernel via .wslconfig.
  - The user approved (a). tools/wsl-hfsplus-ko.sh ran: `fs/hfsplus` built against the WSL tag
    with `/proc/config.gz`, and `insmod` printed HFSPLUS_LOADED. It's not persistent: re-run after
    `wsl --shutdown`.
  - **Root image built** (wsl-mkroot.sh):
    - fsck reports "volume iOSRoot appears to be OK"; 12G of 16G used.
    - Tweaks applied: 7 _PanicOnCrash renamed, 2 CommCenter jobs removed, containers created,
      VirtualAudio disabled, StartRecorderServer off (D21, D211 and Default), and the
      migration/springboard/purplebuddy prefs.
    - Warning: "no mobileactivationd system container in the image; skipped data ark".
    - Output: ~/fw/ipsw-10,5/root/ios16-root.img (16 GiB) and ios16-root.simg (12.1 GB), copied
      to C:\Users\user\ios-mido\rootimg (not in git).
- XNU boot #7 (build #11, run 42188237947, commit 0df6210): read-only eMMC test with
  `rd=ufsroot0` and no `ufsrw`, plus the /product ADT. kscan is clean. Shim jumped.
  - The photo matches build #10: the SWEET drivers run up to `SWEET BATTERY: static fallback`,
    then nothing. There's **no SWEET UFS line and no bsd_init copyright**, so the eMMC code never
    ran and the root image wasn't flashed.
  - Ordering: the AMFI, vnguard and trust-cache lines (kernel_bootstrap_thread, after
    PE_init_iokit) come *before* the SWEET platform expert. The SWEET drivers start on IOKit
    matching threads after PE_lockdown_iokit → StartIOKitMatching.
  - So kernel_bootstrap_thread is stuck, or never rescheduled, between trust_cache_init and
    bsd_init: LOCKDOWN startup, OSKextRemoveKextBootstrap, machine_lockdown, PE_lockdown_iokit,
    vm_set_restrictions or kperf_init_early.
  - Build #12 adds diagnostics:
    - kernel_bootstrap_log and _thread_log print "KB:/KBT: <stage>" on the console;
    - bsd_init_kprintf becomes printf;
    - a 5 s thread_call heartbeat prints "MIDO tick N: bootstrap at <last stage>".
    If there are no ticks, timer wakeups are broken after the scheduler starts. If ticks show a
    fixed stage, that's the call that's stuck.
- XNU boot #8 (build #12, run 42190525335, commit 1ec2523) gave a **panic** with a full panic log:
  `write_pte_fast: PTE write is unaligned, ptep=0xffffffe0…, pte=0x4200000a2e54ec3 @pmap.c:623`.
  - kscan backtrace: kernel_bootstrap_thread+0x374 → vm_commpage_init → commpage_populate →
    pmap_enter_options_addr → pmap_enter_pte → write_pte_fast.
  - The other panic-log fields: Kernel text base 0xfffffff007004000, "CORE 0 is the one that
    panicked", 13 threads, compressor 0%.
  - Cause: arm_init.c picks the advertised page size from RAM. With memSize > 1 GB it sets
    PAGE_SHIFT_CONST = PAGE_MAX_SHIFT (14), i.e. 16 KB VM pages over the 4 KB granule. That's the
    A9 PAGE_RATIO 4 mode; it's also why vm_page_bootstrap showed 95402 pages (×16K = 1.5 GB). This
    ratio-4 path panics while mapping the commpage.
  - Fix: under MIDO_BOARD use the ≤ 1 GB path (PAGE_SHIFT_CONST = ARM_PGSHIFT = 12), so VM page =
    hardware page = 4 KB. Boot-arg `mido_vm16k=1` restores the A9 mode. iOS userland reads
    vm_page_size at runtime, and its 16 KB-aligned segments map fine on 4 KB pages. → build #13.
- XNU boot #9 (build #13, run 42192383661, commit 2911552). The user reports
  **"MIDO ticks: bootstrap at PE_lockdown_iokit"**.
  - The commpage panic is gone, and timers and the scheduler work: the thread_call heartbeat keeps
    firing.
  - The bootstrap thread never logs another stage. Nothing is logged between PE_lockdown_iokit
    and bsd_init's first printf, so the stall is in StartIOKitMatching (registerService is
    async), vm_set_restrictions, kperf_init_early, or the start of bsd_init (throttle_init).
  - Build #14 adds:
    - "KB: PE_lockdown_iokit done" and "KB: bsd_init" marks;
    - from tick 3 on, the heartbeat prints the bootstrap thread's state, wait_event and saved lr,
      plus a frame-pointer backtrace from its kernel context (kstackptr →
      thread_kernel_state.machine.ss), bounded to its kernel stack. Symbolize it with kscan.py.
  - Build #14 failed to compile: struct thread_kernel_state was redefined, because startup.c
    already includes that header and it has no #define guard. Build #15 (6785c0a) dropped the
    include.
- XNU boot #10 (build #15, run 42195525974): "KB: PE_lockdown_iokit done", no "KB: bsd_init".
  - Ticks show `MIDO boot thread: state 0x9 wait 0xfffffff007c39dcc lr fffffff0074329e0`.
    0x9 = TH_WAIT|TH_UNINT.
  - kscan backtrace: machine_switch_context ← thread_invoke ← thread_block_reason ← lck_mtx_sleep
    ← **ml_wait_max_cpus** ← vm_set_restrictions (static, shown as itoa+0x1e0) ←
    kernel_bootstrap_thread+0x650.
  - No "SWEET CPU:" line on screen at all. ml_set_max_cpus is only called by SweetCPU::startAll,
    after every selected CPU passed initCPU and a self-SGI. **SweetCPU never matched**: its
    KernelConfigTables personality is `IONameMatch 'qcom,kryo470'`, and our CPU nodes are
    `arm,cortex-a53`.
  - Its start() would also have panicked on mido's MPIDRs, since it requires Aff0 = 0 and mido
    has 0x000-0x003 / 0x100-0x103.
  - Fix (build #16), all under MIDO:
    - IONameMatch 'arm,cortex-a53';
    - MPIDR check `(phys_id & ~0x103) == 0`;
    - cpuName "Cortex-A53".
    The SGI is already redirected to mido_gic_send_sgi (GICv2 GICD_SGIR).
- Build #16's first run was never picked up: GitHub said "job was not acquired … capacity
  constraints" for macOS. `gh run rerun` built it.
- **XNU boot #11 (build #16, run 42197372970, commit 8948816): M2 reached, eMMC driver works.**
  - CPU: "SWEET CPU: requested=1 selected=1 (limit=8)", "cpu=0 mpidr=0x0 came alive; SGI1 OK",
    "1/1 CPUs initialized, SGI1 verified".
  - Then "KB: bsd_init", the BSD copyright, and every bsd_init step: kauth, MAC Framework,
    bufferinit ("7670 buffer headers and 5883 cluster IO buffer headers"), IOKitInitializeTime,
    eventhandler, aio, pshm, psem, nwk_wq, dlil, socket, domain, skywalk, memorystatus,
    sysctl_mib, bsd_autoconf, loopattach (lo0, gif0, stf0), ether_family, net_init_run,
    inittodr, setconf.
  - **`SWEET UFS: "userdata" on LUN 0: first block 0x148082, 24571 MiB, read-only, DMA at
    0xa4800000`**: the GPT was read over SDHCI PIO and the partition size matches (23-24 GB).
  - `BSD root: ufsroot0, major 1, minor 0`, then vfs_mountroot gives
    `hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : 0` → `cannot mount root,
    errno = 19` → mount(2..5) failed, retried every ~5 s. This is expected, since userdata still
    holds Android's ext4.
  - Heartbeat: the boot thread waits in the mountroot retry. kscan wasn't needed.
  - Next: flash rootimg/ios16-root.simg to userdata, then boot with ios-sweet's userland
    boot-args plus `ufsrw=1 rootrw=1` (the write guard keeps writes inside userdata). A read of
    the HFS+ signature "H+" will also verify that eMMC reads return real data.
- **Root flashed** (user approved): `fastboot getvar partition-size:userdata` = 0x5ffb79e00
  (24571 MiB, matches the driver). `fastboot -S 512M flash userdata rootimg\ios16-root.simg`
  erased, then sent and wrote 24/24 chunks, all OKAY, in 506.9 s (log: xnu\flash-userdata.log).
- **First iOS root boot** (build #16 kernel, shim\out\shim-boot-ios16.img), cmdline:
  `debug=0xc serial=3 cpus=1 keepsyms=1 -v -enable_kprintf_spam -noprogress rd=ufsroot0
  unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1
  launchd_unsecure_cache=1 wdt=-1`. Booted with fastboot boot.
  - **The root mounts read-write**: `SWEET UFS: "userdata" … read-write, DMA at 0xa4806000`,
    `SWEET LOG: console log to the last 4100 KiB of the partition (block 0x7477fa)`,
    `hfs: Initializing the journal (joffset 0x82000 sz 0x800000)`, and
    `hfs: iOSRoot: hfs_recording_init: creating the hotfile btree`. So eMMC reads and writes
    work. The DLIL and Skywalk self-tests passed and the rest of bsd_init completed.
  - **launchd (pid 1) ran; dyld failed**:
    `initproc failed to start -- exit reason namespace 6 subcode 0x1 description: Library not
    loaded: /usr/lib/libSystem.B.dylib Referenced from: <5C15DB07-…> /sbin/launchd Reason:
    tried: '/usr/lib/libSystem.B.dylib' (no such file, no dyld cache), '/usr/local/lib/…'
    (no such file)`. Panicked task pid 1 launchd, 39 pages, 1 thread.
  - The image is fine (tools/wsl-rootcheck.sh): System/Library/Caches/com.apple.dyld has
    dyld_shared_cache_arm64 (header `dyld_v1   arm64`) plus subcaches, ~3.1 GB, hard-linked
    into the OS cryptex path. So dyld found the files but didn't get a usable shared region.
  - Main suspect: 4 KB VM pages against a cache laid out and code-signed for 16 KB pages
    (shared_region_map_and_slide_2_np, or F_ADDFILESIGS_RETURN / cs_blob page size). The kernel
    printf()s shared-region errors and the SWEET log ring was on during this boot, so the next
    step is tools/sweetlog.ps1: boot the stock recovery into RAM, dd the last 1025×4K of
    userdata, decode with sweet-log.py.
  - **Confirmed without the log** (tools/cache-cdpage.py on the image):
    - Every dyld_shared_cache_arm64* file has a CodeDirectory v0x20400 with SHA-256 and
      **pageShift 14** (16 KB). /sbin/launchd has pageShift 12, which is why it loaded.
    - bsd/kern/ubc_subr.c rejects `cd->pageSize != 12 && != PAGE_SHIFT` with "disallowing
      unsupported code signature page shift", so with 4 KB VM pages the cache's signature is
      refused. 4 KB pages can't work for iOS 16 userland.
  - **Back to 16 KB VM over the 4 KB granule** (XNU's own > 1 GB choice, the A9 PAGE_RATIO 4
    mode), and the build #12 commpage panic is fixed:
    - pmap_create_commpages picks the commpage text VA as `random() % num_ptes` over the 512
      hardware 4 KB leaf slots, but write_pte_fast writes 4 PTEs and needs a 16 KB-aligned VA.
      That only lands one time in four, hence "PTE write is unaligned".
    - Fix: `text_leaf_index &= ~(PAGE_RATIO - 1)`.
    - arm_init: MIDO keeps 16 KB by default; boot-arg `mido_vm4k=1` forces 4 KB.
    → build #17.
- **XNU boot #13 (build #17, run 42204141481, commit 288abcf), same iOS boot-args: iOS 16
  USERLAND RUNS, up to SpringBoard. No panic.** (8 photos, about 2 minutes of boot.)
  - launchd boot tasks ran: xpcroleaccountd, init_featureflags, auearlyboot ("No Early Boot
    Accessories … End Early Boot"), tzinit ("Bad zoneinfo link" is harmless), FinishRestoreFromBackup,
    finish_demo_restore, sysstatuscheck, prng_seedctl ("failed to load kernel prng seed", "failed
    to load virtual random", both expected), launchd_cache_loader.
  - launchd_cache_loader: "Using unsecure cache: /System/Library/xpc/launchd.plist … Cache sent to
    launchd successfully". launchd UUID 5C15DB07-B3BC-3DF3-B62F-3E498B993B8E: **"Early boot
    complete. Continuing system boot."**
  - Daemons exec'd, roughly in order: SpringBoard, many xpcproxy, keybagd, usermanagerd ("SWEET AKS:
    keybagd sel 0 … -> 0x0"), UserEventAgent, mediaserverd, backboardd, remoted, configd,
    mobiletimerd, locationd, nfcd, timed, sleepd, driverkitd, mtmergeprops, containermanagerd,
    fseventsd, thermalmonitord, sharingd, rapportd, logd, notifyd, ptpd, lockdownd, DumpPanic,
    IOMFB_FDR_Loader, fairplayd.H2, distnoted, bluetoothd, imagent, identityservicesd, amfid,
    AppleCredentialManagerDaemon, watchdogd, logd_helper, misd, remindd, cloudpaird, healthd,
    accessoryupdaterd, seld, OTACrashCopier, wcd, routined, magicswitchd, atc, dasd,
    peakpowermanagerd, runningboardd, bluetoothuserd, safetyalertsd, WirelessRadioManagerd,
    ManagedSettingsAgent, powerd, amsaccountsd, assistantd, familynotificationd.
  - ios-sweet's process sampler ("SMP --- sample 0") shows backboardd[56] (pc 0x1cd793180) and
    SpringBoard[26] (pc 0x1f1cdfd08) with user backtraces in the shared cache (slide 0x4d6cc000).
  - So all of these work: 16 KB VM over the 4 KB granule, the commpage fix, dyld and the shared
    cache, the EL0 LSE emulator (every daemon uses libdispatch/pthread atomics), eMMC
    read/write at 25 MHz 1-bit PIO, timers, GICv2 and cpus=1.
  - The heartbeat still prints "MIDO tick N: bootstrap at bsd_init" (the stage is stale once
    bsd_init turns into the init process). It's noise now and should be removed or limited.
  - Open: is SpringBoard drawing? Kernel text is drawn over the framebuffer. ios-sweet uses
    `sweetfb_noconsole=1` to show the GUI and keeps the console in the persistent log. Also,
    SWEET FB "presented as 1125x2436" (iPhone X, patch 0061) doesn't match the iPhone10,5
    identity (1242x2208 canvas, 1080x1920 panel).
- **Later in the build #17 boot: eMMC write errors.**
  - `MIDO EMMC: CMD25: transfer error (sts 0x00108002)`: error bits 0x0010 = SDHCI data timeout
    while the card was busy programming (the data had all been sent).
  - `jnl: … strategy err 0x5`, then `end_transaction: only wrote 20480 of 45056 bytes`.
  - After that, a cascade: containermanagerd couldn't create the system containers, then
    **backboardd** threw `NSInvalidArgumentException -[NSURL initFileURLWithPath:]: nil string`
    and crash-looped ("Too many corpses"); lockdownd, lsd and driverkitd crashed too.
  - Disassembly (tools/wsl-bbdis.sh, ipsw macho disass) of backboardd at 0x10001d858:
    `BSPathProviderFactory pathProviderForSystemContainerForCurrentProcess` → `cachesPath` is nil.
- Build #18 (d0ad1df; the first run had no runner, then 5e141d1, run 42209131940):
  - eMMC: data timeout counter set to the maximum (CLK[19:16] = 0xE) in mido_take_over;
    mido_recover() = reset lines, CMD13, CMD12 if in data/rcv state, wait for tran; each chunk is
    retried up to 3 times.
  - The heartbeat prints only on a stage change or once a minute.
  - New boot-arg `mido_mute=1` mutes the drawn console from kernel_bootstrap_thread.
    sweetfb_noconsole only mutes at the first GUI swap.
- XNU boot #15 (build #18; root re-flashed clean; `sweetfb_noconsole=1 mido_mute=1`). The user
  saw only early text for 10 minutes. Lines from -enable_kprintf_spam still draw after the mute,
  so drop that boot-arg.
  - **Persistent log pulled** with tools/sweetlog.ps1, using the user's OrangeFox
    (ios-mido\recovery.img) booted into RAM: version 1, 762756 bytes, flush #46.
  - **0 MIDO EMMC errors, no journal errors, no backboardd crash.** The only crash is driverkitd.
    SpringBoard is alive (AKS calls).
  - New blocker: backboardd opens AppleCLCD and creates surfaces **1242x2208** (the D211 canvas)
    and 1125x2436 (the presented size), then loops `IOServiceMatching failed for:
    AppleM2ScalerCSCDriver` (×9838). The presented size ≠ the canvas, so it wants the hardware
    scaler, and no frame is ever presented.
  - Fix (build #19), MIDO only in SweetFramebuffer.cpp:
    - present **1242x2208**;
    - `AppleCLCD::midoPresentScaled()` maps each panel pixel (1080x1920) to its canvas pixel
      with 16.16 fixed-point nearest neighbour (0.87x, the real 8 Plus ratio), bounds-checked
      like the crop path;
    - `sweetfb_native=1` still presents the panel size.
  - Not yet adjusted: SweetHIDTouch's panel→presented mapping (no mido touch driver yet).
- **XNU boot #16 (GitHub run #22, run id 42215454774, commit f212e27): THE APPLE LOGO IS ON
  THE SCREEN** (user report).
  - Setup: clean root re-flashed (506.8 s, 24/24 OK). Boot-args = the iOS set,
    `sweetfb_noconsole=1 mido_mute=1`, without -enable_kprintf_spam. The console stopped at
    "MIDO: console muted (mido_mute=1)".
  - The Apple logo appeared **about 8 minutes** after boot. So backboardd presents frames through
    AppleCLCD and the 1242x2208 → 1080x1920 CPU downsampling works. **Milestone M3 (Apple logo).**
  - Numbering note: from here on, builds are referred to by GitHub run number (#22 = what this
    log called build #19; earlier failed/unacquired runs and reruns also count).
  - Photo: white Apple logo, horizontally centred, at ~40% of the screen height (slightly
    high; to check the vertical mapping). Then a black screen with the backlight on, then the Apple
    logo again (a backboardd/SpringBoard restart during first boot), then:
  - **THE iOS 16 LOCK SCREEN** (user report): clock and date, full battery icon, "iPhone"
    carrier text at the top left. **Milestone M4 (lock screen).** The clock shows **red**, so
    a possible R/B channel swap needs checking against a photo. Touch and buttons are not
    wired for mido yet.
  - Lock screen photo: the layout and scale are right (status bar, "iPhone", charging battery
    in green, date, 4:42, clock running). But the background is dark navy/purple, the clock is
    peach with a red-orange glow, and the white text looks slightly yellow.
  - Shim colour test (shim.c, shim-colortest.img, no XNU): bars written as 0xffRRGGBB (memory
    B,G,R,X = iOS BGRA) show **red, green, blue** in that order, so the MDP RGB0 unpack
    (0x03020001) is correct.
  - Cause: backboardd's surfaces are `fmt 0x62336138` = **'b3a8' =
    kCVPixelFormatType_30RGBLE_8A** (2 planes; plane 0 = R[29:20] G[19:10] B[9:0] in extended
    range 384 = 0.0 .. 895 = 1.0, plane 1 = alpha). The D211 has a P3 panel, so backboardd uses
    wide colour. Copied raw as BGRA: black (384,384,384) → R6 G1 B128 (navy), white (895×3) → R253
    G255 B127 (pale yellow). This exactly matches the photo.
  - Fix: midoPresentScaled converts 'b3a8' pixels through a 1024-entry XR10 → 8-bit table
    (clamped (v-384)*255/511) and logs "SWEET FB: converting b3a8 …" once. 8-bit surfaces are
    still copied raw. (Run #23 had only this; it was cancelled at the user's request so it could
    ship together with input.)
- **Input bring-up (touch + buttons), developed in the shim first** (shim/input.c,
  shim-inputtest.img, no XNU).
  - Hardware from mido.dtb, plus a read-only OrangeFox probe (tools/fox-hwprobe.sh). **Never
    `cat /sys/kernel/debug/gpio` on MSM8953**: it dumps secure GPIOs and the XPU violation
    resets the phone (it happened once).
    - FT5435 `focaltech,5435` @0x38 on i2c@78b7000 (BLSP1 QUP3, qcom,i2c-msm-v2, 400 kHz);
      reset gpio64, irq gpio65, 1080x1920; virtual keys at y=2040: x=200 menu(139),
      500 home(172), 800 back(158).
    - Supplies: vdd = pm8953_l10 2.85 V, vcc_i2c = pm8953_l6 1.8 V. OrangeFox has both enabled,
      but **after LK, L10 is off**.
    - Keys: vol_up = TLMM gpio85, active low (gpio-keys); power = PON KPDPWR, vol-down = PON
      RESIN (qpnp-power-on @0x800).
  - Test run 1: QUP reachable (HW 0x20070000; clocks on after the GCC_APCS vote bit 10 and the
    QUP3 I2C CBCR 0x01804020). **All 3 keys toggle** (PON INT_RT_STS 0x810 b0/b1, gpio85 IN).
    No I2C answer: L10 off (ST=00 EN=00, V=0x54).
  - Test run 2: a direct SPMI write to L10 EN_CTL **reset the phone instantly** (RPM-owned
    peripheral, arbiter violation).
  - Test run 3: shim/rpm.c, a minimal RPM client like LK smd.c/rpm-smd.c.
    - SMEM 0x86300000 legacy TOC; item 13 alloc table, `rpm_requests` (edge 0xf) **cid 4**;
      port info at item 14+cid, FIFOs at item 338+cid (0x400 bytes each way).
    - LK leaves the channel closed (TX 0 / RX 4); reset → OPENING → OPENED gives TX 2 / RX 2.
    - Request `req` ldoa 10 {swen=1, uv=2850000, ma=40}; IPC doorbell 0x0b011008 bit 0.
      **ACK 40 bytes**, then L10 ST=82 **VREG OK**, V=0x58 (2.85 V).
    - Pin-mux search: blsp_i2c3 is **mux 2** (3 and 1 give I2C status errors). **FT5435
      reg 0xA3 = 0x54**, 0xA8 = 0x3B.
    - Touch reads from 0x02 (13 bytes): the first ~4 reads are all 0xFF while the firmware
      boots, then a real drag trace (538,1375 → … → 974,1873 → 953,23), N=0 on lift. Keys
      also work.
  - shim `input_setup()` before every XNU jump: RPM L10, clocks, gpio10/11 mux 2, gpio65/85
    pull-up inputs, reset pulse on gpio64. The RPM vote and the pin state persist into XNU.
  - XNU `bsd/dev/mido_input.c` (mido/xnu/src/, installed by mido_xnu.py `create()`; mkpatch.sh
    now `git add -N` so new files are in the diff), read-only:
    - QUP v1 FIFO I2C; PON RT via SPMI observer EXT_READL; gpio65 IRQ / gpio85 vol+ reads.
    - Thread polls every 10–15 ms; reads touch only while the IRQ is low or something is down.
    - ≤ 2 fingers; the y ≥ 1920 key row is the centre key → Home.
    - Events go to sweet_hid_touch / sweet_hid_key (0 power, 1 vol+, 2 vol-, 3 home).
    - Started from sweet_ufs_init where ios-sweet starts its SD730 input. Boot-arg `midoinput=0`
      disables it.
  - SweetHID (MIDO): touches are normalised against the 1080x1920 panel (= the whole downsampled
    canvas), y clamped at 1919, SurfaceHeight 121 mm.
- Commit 826be09 was built as run #24, but the job was "not acquired" twice (arm64 macOS runner
  capacity). **Run #25 = the same commit on `runner=macos-15-intel`**: picked up at once, built
  in 15 min, and the kernel is identical in content (kscan clean; MIDO INPUT, b3a8, scaling, mute
  and LSE strings all present). Intel is the fallback runner from now on.
- **XNU boot #17 (run #25)** on the root from the previous boot. User: lock screen, touch and
  buttons work **but react after minutes**; the clock is still red. Persistent log (230157 bytes,
  flush #87):
  - `MIDO INPUT: FT5435 id 0x54 (err 0), PON chan 44, vol+ gpio85=1`; **82 input events logged
    promptly**: touches, home key, power (0), vol+ (1), vol- (2). So the kernel driver is
    fine and the latency is downstream.
  - Frames this boot are **'BGRA' (0x42475241)** 1242x2208 (not b3a8), so the red clock is
    iOS's own GPU-less rendering of the clock effect, not a colour bug.
  - `display off` / `display on`: auto-lock. Only crash: driverkitd. `SWEET FB: 1000/2000/3000
    swaps`.
  - 15 `MIDO EMMC: CMD18 … data timeout (sts 0x00008000)` → `ok on retry 1`. The hardware error
    bits are 0: this is the software 1 s deadline expiring while the thread was preempted.
    **Fix: re-check readiness after the deadline; longer deadlines.**
  - Diagnosis: the single A53 core is saturated by AppleCLCD::present(): a full 1080x1920
    gather/downscale per flush at sweetfb_hz=10 (default) from the vsync timer, with thousands
    of swaps. Test: the same kernel with `sweetfb_hz=1` (shim-boot-run25-hz1.img).
    Planned fixes:
    - present only new frames;
    - per-row change detection / damage rects;
    - higher priority for the input thread;
    - then eMMC 8-bit/HS + DMA, CPU clock and SMP.
  - **sweetfb_hz=1 test (same kernel): still extremely slow**, so the panel copy is not the main
    load.
  - SweetFramebuffer sends vsync notifications at a fixed 60 Hz (SWEET_FB_VSYNC_US 16667), and
    backboardd renders on vsync in software, hence the >3000 swaps.
  - The sampler only ever catches SpringBoard (102) and backboardd (97). The earlier log has
    `logd_helper` "burning CPU": 90 s system vs 0.12 s user, i.e. kernel time, consistent with
    LSE-emulation spin storms.
  - Next build:
    - `mido_vsync_hz` (default **15**) with the panel copy per vsync;
    - `MIDO LSE:` per-minute report (total, delta, top-3 processes from an 8-slot decaying
      table);
    - eMMC deadline re-check (per-block 3 s, and status re-read after the deadline);
    - input thread at kernel priority 81.
- Run #26 failed: kernel_thread_start_priority was implicitly declared in BSD; run #27
  (f95e428, Intel runner) builds.
  - Boot: user reports "basically the same".
  - Log: `SWEET FB: MIDO vsync 15 Hz`, **`MIDO LSE: 0 emulated` every minute for the whole boot**,
    0 MIDO EMMC errors, 28 input events, crashes only gpsd/driverkitd.
  - So the iPhone 8 userland never hits the LSE emulator (the Pixel port says the same).
- **Reference port: ios-sailfish** (github.com/joranv1/ios-sailfish, Pixel 2016, Snapdragon 821,
  Adreno 530, fork of ios-sweet, **no licence file**: study only, our own code), cloned to
  research/.
  - It has: Adreno 530 kernel driver + Mesa freedreno GLES in backboardd (sweetgl.dylib, EAGL
    shim, DYLD_INSERT via sweetenv); RPM GLINK for rails, clocks and DDR (0145-0147: the RPM
    ignores rate votes until `clk0` id 2 `Enab`=1, then every RPM clock must be voted at max);
    CPU PLL / SAW turbo (0093, 0102, 0183).
  - Boots to the lock screen in ~20 s.
- **CPU clock (shim perf.c)**:
  - **LK leaves both A53 clusters on GPLL0 = 799 MHz** (mux CFG 0x401 src 4), the CPU PLL off,
    and the CCI on GPLL0/2.
  - APC rail PM8953 S5 (PPID 0x120) V41=0x9e ≈ 0.87 V.
  - apcs_cpu_pll (Linux clock-cpu-8953): L at +0x08, MODE BYPASSNL→RESET_N→LOCK(b31)→OUTCTRL,
    CONFIG 0x200d4828/0x6, mux src 5.
  - Test 2: L=54 → 1036 MHz OK. Test 3 sweep at the unchanged voltage, workload checksum ×3 per
    step: **1401, 1689, 1804, 1958, 2015 MHz all OK**.
  - Every XNU boot now runs perf_boost(): sweep to the highest passing step (fallback on a
    mismatch) + CCI to GPLL0/1 = 800 MHz. Image shim-boot-run27-boost.img (run #27 kernel,
    unchanged).
  - Caveat: no CPR or thermal management; short single-core test while cool.

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

## 2026-11-06 evening: eMMC fast mode, GPU power-up (shim tests)

### eMMC 8-bit / HS 50 MHz
- Shim `sdhc_fast()`: EXT_CSD (rev 8, card type 0x57), CMD6 bus width 8, then HS_TIMING
  + GCC SDCC1 RCG GPLL0/16 = 50 MHz. 1 MiB of CMD17 reads: 654 ms -> 337 ms.
- The first version broke writes (LOG WRITE data error, code 5) while reads were fine.
  Each step now also rewrites one log block with its own data and reads it back. A
  failed write test falls back to the last good mode.
- Final: `EMMC FAST MODE=2` (8-bit HS 50 MHz, writes verified). `sdhc_fast()` runs on
  every boot before the XNU loader. XNU's transport only resets lines, so it keeps
  the bus mode.

### Adreno 506 power-up: what it took (in order)
1. gfx3d RCG to XO, enable the GCC gfx branches (0x01859020..0x48), CX GDSC
   (0x0185904c) on.
2. **GX rail = msm_gfx_ldo (0x0185f000) in BHS mode, under GDSC control** (Linux
   msm_gfx_ldo_init + enable_bhs_mode): unclamp (b31), CPR auto-bypass (b29),
   msm8953_ldo_config table, then LDO under SW, PWR_SRC=BHS, BHS_EN_FEW/REST and
   BHS_UNDER_SW_CTL cleared. Final PWRSWITCH_CTL 0x20000038, identical to Linux
   (checked in OrangeFox debugfs msm_gfx_ldo/debug_info with the GPU forced on).
   Driving the BHS from software never got the GDSC ack. The fake ack (ACK_SW_OVR)
   made GX report ON without real power.
3. **GX_DOMAIN_MISC (0x0185b00c) bit 0 = OXILI_GMEM_CLAMP_IO cleared** (Linux
   msm_gcc_gfx_probe). Without it GX never came up for real. After it, the GX GDSC
   comes on through the GDSC-controlled BHS and gfx3d CBCR drops CLK_OFF.
4. **TZ scm_restore_sec_cfg(TZ_DEVICE_GPU = 18, 0)** right after CX is on: SMC64
   x0=0x42000c02, x1=SCM_ARGS(2)=2, x2=18, x3=0 -> ret 0. Linux arm-smmu does this on
   every CX enable (qcom,enable-static-cb + enable-smmu-halt). Before it, every read of
   the GPU (0x01c00000) or its SMMU (0x01c40000) took a synchronous external abort
   (ESR 0x96000210).
- Result: RBBM_STATUS=1, RBBM_INT0=0, CP_HW_FAULT=0, VSC_BIN_SIZE=0x101,
  GPU SMMU+0 (sCR0 NS view)=0x00200436. **The GPU answers.**
- Do NOT read GPU SMMU +0x400 (secure register space): it resets the SoC.
- Tools: a fault-tolerant probe read (exception slot 4 skips the load while
  gpu_probe_active), and the shim log pulled through OrangeFox:
  `adb exec-out dd if=/dev/block/bootdevice/by-name/cache bs=512 count=128`.
- Next: GPU stage 2, CP microcode (a530_pm4/pfp) + ring buffer NOP/scratch test, then
  the zap shader (PAS via TZ) and an XNU GPU driver.

### GPU stage 2: the command processor runs (2026-11-06)
- Shim test image `shim-gpustage2.img` (`build.py gpufw`: the ramdisk is a "MIDOGPU1" blob
  of a530_pm4.fw + a530_pfp.fw from fw/gpu, never committed).
- Firmware copied without its 4-byte header to 0xa0008000 / 0xa0010000, ring at 0xa0000000,
  scratch at 0xa0004000 (the shim runs with the MMU and caches off, so no cache maintenance).
- GPU SMMU: IDR1 0x20008004 -> context banks at +0x8000. CB0 (gfx3d_user, non-secure) SCTLR=0
  after the TZ restore, so the GPU's addresses pass untranslated. TRUST_CNTL already 0.
- a5xx_hw_init subset (A510-size CP thresholds), CP_ME_INIT (8 dwords, no microcode
  workarounds), CP_MEM_WRITE 0xc0ffee01, NOP. Result: RPTR=WPTR=14, HW_FAULT=0,
  **SCRATCH=0xC0FFEE01: the command processor works.** No zap shader was needed for this.
- The first attempt reset the SoC after the CP start. The second, with 5 ms after the halt
  release and 2 ms after WPTR (and a log flush in between), passed.
- Next: make the GPU draw something visible (2D blit/fill into the framebuffer at
  0x90001000), then move the bring-up into an XNU driver.

### GPU stage 3: the Adreno 506 draws on the screen (2026-11-06, late evening)
The GPU 2D engine copied a CPU-drawn 256x256 gradient onto the panel. Confirmed by the user
on the phone. What it took after stage 2 (in order of discovery):
1. UCHE init from a5xx_hw_init (cache ways, trap/write-through base, GMEM range), every
   *_ADDR_MODE_CNTL = 1, Mesa fd5_emit_sysmem_prep state (render mode BYPASS, IB2 skip off,
   CCU colour invalidate, PC/VFD power, **RB_CCU_CNTL = 0x10000000**), then fd5_blitter
   emit_setup + per-row CP_BLIT (the 4320-byte framebuffer pitch is not a multiple of 64)
   and PC_CCU_FLUSH_COLOR_TS + CACHE_FLUSH_TS. Not enough on its own: the blits executed
   with no fault and wrote nothing, even into ordinary RAM.
2. Cause: the GPU starts in **secure mode**; render traffic goes out on the secure stream.
   CP_SET_SECURE_MODE 0 without a zap shader = CP opcode error (CPINT=1).
3. **Zap shader** (pas-id 13, a506_zap.mdt + .b02) through TZ PIL, as msm subsys-pil-tz /
   peripheral-loader do: metadata at the region start, INIT_IMAGE (SCM_ARGS(2,VAL,RW)),
   MEM_SETUP (start, ALIGN(end,4K) - start = 0x2000), load .b02, AUTH_AND_RESET.
   AUTH kept failing (-2 / -15) until the **secure SMMU page-table pool** was given to TZ
   (msm_iommu_sec_pgtbl_init: MP svc PTBL_SIZE 3 -> 0x21000, PTBL_INIT 4 (paddr RW, size)).
   Linux does this once at boot for any SMMU with qcom,tz-device-id. Reference: OrangeFox
   with firmware_class.path=/tmp/fw loaded the same zap ("Brought out of reset", 0x8f800000).
4. After the zap: ME_INIT, then **CP_SET_SECURE_MODE 0** before any memory write.
   RBBM_SECVID_* (0xf4xx/0xf8xx) become secure-only: a read is an external abort.
- Pools used by the shim test: secure PTBL 0x8c000000 (0x21000), zap 0x8f800000 (0x2000),
  GPU ring/scratch/fw/IB/surfaces at 0xa0000000+.
- Next: move power-up + zap + CP init into an XNU GPU driver, then an msm-style
  BO/submit interface for Mesa freedreno in backboardd.

### GPU in XNU: bring-up driver works on the first boot (2026-11-06, night)
- `bsd/dev/mido_gpu.c` (repo mido/xnu/src, commit b96bfeb, CI run 42296201174), boot-arg
  `midogpu=1`; firmware via `mkpayload.py --blob GPU-PM4/GPU-PFP/GPU-ZAP-MDT/GPU-ZAP-SEG`
  (files from fw/gpu, never committed). Image: shim/out/shim-boot-gpu1.img (image B
  command line + midogpu=1, shim with eMMC HS50).
- Fixed physical windows outside XNU RAM, mapped uncached with ml_io_map: secure PTBL
  0x98000000, zap 0x98800000, ring/fw/test buffers 0x99000000.
- Persistent log (tools/sweetlog.ps1): CX on, TZ restore 0, GX on, secure PTBL 0x21000 ok,
  zap AUTH_AND_RESET 0, "CP runs in 1 ms", "2D blit done in 1 ms: 65536 of 65536 pixels
  match", "MIDO GPU: READY". iOS reached the lock screen as before (rendering is still CPU).
- eMMC HS50 boot (shim-boot-run27-B60smp4-emmc50.img): user: "approximately the same time,
  maybe a bit faster". Boot is bound by the CPU and the PIO transport, not the bus clock.
- Next: a user-space submit interface (msm-style BO + submit + fence) for Mesa freedreno,
  then Mesa in backboardd behind an EAGL shim, then a GPU present path for the display.

### GPU step 2: user-space submission works (2026-11-06, night)
- Kernel: MidoGPU IOService + user client (commit after b163ddb, CI run 42299844836):
  GetParam / BONew / BOFree / Submit / Wait / Log; BOs from the 16 KB-page pool at
  0x99400000.., mapped write-combined via clientMemoryForType; submission = IB + CCU colour
  flush + CACHE_FLUSH_TS fence into the 16 KB ring (word wrap).
- User side: mido/tools/midogpu-test.c (built + ad-hoc signed by the xnu workflow),
  installed with mido/tools/wsl-add-gpu-test.sh (binary in /usr/local/bin, job added to
  /System/Library/xpc/launchd.plist by add-launchd-job.py). Root image
  rootimg/ios16-root-gpu.simg flashed to userdata (user approved; 500 s, 24/24 OKAY).
- Boot shim-boot-gpu2.img: "MIDO GPU USER: midogpu-test: opened MidoGPU, chip id 05000600",
  BOs 0x99400000/0x99440000/0x99480000 mapped at 0x100504000.., "submit fence 1 signalled,
  65536 of 65536 pixels match -> USER-SPACE GPU SUBMISSION WORKS".
- That first boot of the fresh root sat on the second Apple logo when the user stopped it
  (log tick 180, SpringBoard/backboardd alive, the usual bluetoothd/fairplayd restarts);
  run27's first boot needed tick ~216. Probably just the slow first boot; recheck.
- Next: Mesa freedreno for iOS on top of this interface (libdrm-msm style shim), then
  backboardd/CoreAnimation through an EAGL replacement, then GPU present.

### GPU step 3 started: Mesa freedreno for iOS (handoff)
- Plan (approach after ios-sailfish, own code): Mesa 25.3.6 static, gallium freedreno +
  GLES2, no EGL/GLX/platforms/LLVM; screen via fd_drm_screen_create_renderonly on a fake
  fd; contexts via st_api_create_context; a midogl library emulating the libdrm/MSM ioctls
  on MidoGPU (BO new/info/map, submit, wait fence, get param).
- libdrm calls freedreno needs: drmCommandWrite/WriteRead, drmIoctl, drmGetVersion/
  drmFreeVersion, drmGetCap, drmPrime{HandleToFD,FDToHandle}, drmOpenWithType, drmSyncobj*.
- Mesa changes needed: build src/freedreno/drm on darwin (src/freedreno/meson.build
  `system_has_kms_drm` gate; meson.build sets it false for darwin), route
  fd_bo_map_os_mmap (freedreno_bo.c:613, os_mmap) to the midogl BO map.
- Sparse checkout of mesa-25.3.6 in research/mesa-25.3 (src/freedreno/drm present;
  src/freedreno/meson.build still to add to the sparse set).
- Next: mido/gl/{mesa-ios.sh, include/xf86drm.h, mesa.patch} + a CI job that builds the
  Mesa static libs for iOS; then midogl + a GLES triangle test via midogpu-style job.
- Pending check: second boot of shim-boot-gpu2.img should reach the lock screen.

### GPU step 3: OpenGL ES triangle through Mesa freedreno on iOS (2026-11-08)
- Mesa 25.3.6 cross-built for iOS in CI (.github/workflows/mesa-ios.yml, mido/gl/build-mesa-ios.sh,
  mesa.patch: build src/freedreno/drm on darwin, BO map through midogl, no ELF build-id, sysctl
  for RAM), with static libraries for gallium freedreno + GLES, no EGL/GLX/LLVM.
- mido/gl/midogl_drm.c: libdrm/MSM emulation on MidoGPU ("msm" 1.7, softpin); midogl.c: screen
  and surfaceless contexts (st_api_create_context; frontend get_param is mandatory).
- Boot shim-boot-gpu3.img + root rootimg/ios16-root-gl.simg (64 MiB placeholder
  /usr/local/bin/midogl-test, extents rootimg/gl-extents.txt, updated in place with
  tools/gl-push.ps1 from OrangeFox in ~3 s; md5 verified).
- Log: "GL: OpenGL ES 3.1 Mesa 25.3.6 | FD506 | freedreno", shaders linked, draw GL error 0,
  center ff8000ff, corner 0000ffff, triangle 20808 / clear 44728 / other 0 ->
  "OPENGL ES TRIANGLE DRAWN BY THE GPU".
- Daemons have a small jetsam limit (GLSL builtins killed the test, reason 7):
  memorystatus_control(7 SET_MEMLIMIT_PROPERTIES); commands 5/6 return EINVAL on this kernel.
- GPU still at 19.2 MHz. Next: GPU clocks, then step 4: backboardd + EAGL replacement + GPU present.
