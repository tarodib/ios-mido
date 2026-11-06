# Pull the XNU persistent console log ("SWEETLOG" ring, last 1025 x 4 KiB of userdata)
# from a phone in fastboot: boots the stock recovery (OrangeFox) into RAM only, reads with
# adb exec-out dd (read-only), decodes with ios-sweet's tools/sweet-log.py.
#   tools\sweetlog.ps1 [-Out xnu\sweetlog.txt] [-NoBoot]
param([string]$Out = "C:\Users\user\ios-mido\xnu\sweetlog.txt", [switch]$NoBoot)
$ErrorActionPreference = 'Stop'
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$root = 'C:\Users\user\ios-mido'
$py = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"

if (-not $NoBoot) {
    $fb = (fastboot devices) -join ''
    if (-not $fb) { throw 'phone not in fastboot' }
    fastboot boot "$root\backup\stock\recovery.img" 2>&1 | ForEach-Object { "$_" }
}
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
    $s = (adb get-state 2>$null)
    if ($s -eq 'recovery' -or $s -eq 'device') { break }
    Start-Sleep -Seconds 2
}
$dev = (adb shell 'for d in /dev/block/by-name/userdata /dev/block/bootdevice/by-name/userdata; do [ -e $d ] && readlink -f $d && break; done').Trim()
if (-not $dev) { throw 'userdata block device not found' }
$bytes = [int64]((adb shell "blockdev --getsize64 $dev").Trim())
$skip = [int64][math]::Floor($bytes / 4096) - 1025
$bin = "$Out.bin"
# exec-out via cmd for a binary-safe redirect
cmd /c "adb exec-out ""dd if=$dev bs=4096 skip=$skip count=1025 2>/dev/null"" > ""$bin"""
"{0}: {1} bytes, blocks {2}.. -> {3} bytes" -f $dev, $bytes, $skip, (Get-Item $bin).Length
& $py -I "$root\repo\tools\sweet-log.py" $bin $Out
