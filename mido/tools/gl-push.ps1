# Write a new midogl-test straight into the 64 MiB placeholder on the iOS root, from
# OrangeFox (phone in recovery), so no full userdata flash is needed.
#   tools\gl-push.ps1 path\to\midogl-test
# Extents come from rootimg\gl-extents.txt: "<file block> <disk block> <count>", 4 KiB blocks.
param([Parameter(Mandatory)][string]$Binary)
$ErrorActionPreference = 'Continue'
$size = 64MB
$local = "$env:TEMP\midogl-pad.bin"
[byte[]]$b = [IO.File]::ReadAllBytes($Binary)
if ($b.Length -gt $size) { throw 'binary larger than 64 MiB' }
$p = New-Object byte[] $size
[Array]::Copy($b, $p, $b.Length)
[IO.File]::WriteAllBytes($local, $p)
adb push $local /tmp/midogl.bin | Out-Null
$dev = (adb shell 'readlink -f /dev/block/bootdevice/by-name/userdata').Trim()
foreach ($line in Get-Content C:\Users\user\ios-mido\rootimg\gl-extents.txt) {
    $f = $line.Trim() -split '\s+'
    if ($f.Count -eq 3) {
        adb shell "dd if=/tmp/midogl.bin of=$dev bs=4096 skip=$($f[0]) seek=$($f[1]) count=$($f[2]) conv=notrunc 2>&1 | tail -1"
    }
}
adb shell 'sync; rm /tmp/midogl.bin'
Remove-Item $local
"pushed $($b.Length) bytes"
