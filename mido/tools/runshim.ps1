# Build + fastboot-boot the shim, time its return to fastboot, decode the
# timing side channel (hold = HOLD_BASE s + 4 s * code), then pull the cache log.
# Baseline overhead (LK + reset) measured at ~5 s.
param([switch]$NoBuild, [switch]$NoLog)
$root = Split-Path $PSScriptRoot
$py = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
if (-not $NoBuild) { & $py -I "$root\shim\build.py" | Select-Object -Last 1; if (-not $?) { exit 1 } }
# USB can re-enumerate briefly; give fastboot a few seconds before asking adb to reboot.
$t = 0; while ($t -lt 6 -and -not (fastboot devices)) { Start-Sleep 1; $t++ }
if (-not (fastboot devices)) { adb reboot bootloader 2>$null }
$t = 0; while ($t -lt 60 -and -not (fastboot devices)) { Start-Sleep 2; $t += 2 }
if (-not (fastboot devices)) { 'phone not in fastboot'; exit 1 }
fastboot boot "$root\shim\out\shim-boot.img" 2>$null | Out-Null
$s = Get-Date; Start-Sleep 4
while (((Get-Date) - $s).TotalSeconds -lt 120) { if (fastboot devices) { break }; Start-Sleep 0.5 }
$el = ((Get-Date) - $s).TotalSeconds
if ($el -ge 120) { 'not back in fastboot after 120 s'; adb devices; exit 1 }
$codes = 'ok','bus power stays off','card never reached TRAN','LBA0 read/sig bad','LBA1 not GPT',
         'GPT entry read failed','cache != expected','no cache entry','log write failed','read-back verify failed',
         'CMD1 answered but busy forever','CMD2 failed','CMD3 failed','CMD7 failed','not TRAN after re-init',
         '(15 unused)','CMD1: controller busy','CMD1: cmd timeout (no response)','CMD1: cmd CRC/index error',
         'CMD1: data timeout','CMD1: data error','GCC CFG sanity fail','GCC RCG update never latched'
$holdBase = 30   # must match HOLD_BASE in shim/shim.c
# Measured overhead (LK + reset + log flushes) is ~6.5 s; codes are 4 s apart, so +-2 s margin.
$code = [Math]::Round(($el - 6.5 - $holdBase) / 4)
"back in fastboot after {0:N1} s -> eMMC code ~{1} ({2})" -f $el, $code, $codes[[Math]::Max(0,[Math]::Min(22,$code))]
if (-not $NoLog) { & "$root\tools\shimlog.ps1" }
