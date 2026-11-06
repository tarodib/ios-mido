# Pull the shim's log from the first 128 sectors of the `cache` partition.
# Phone must be in fastboot (where the shim leaves it). Boots OrangeFox from the
# backup (RAM only, nothing flashed), reads the log, prints it.
param([switch]$NoBoot)
$root = Split-Path $PSScriptRoot
$out = "$root\shim\out\shimlog.bin"
Remove-Item $out -ErrorAction SilentlyContinue
if (-not $NoBoot) {
	fastboot boot "$root\backup\stock\recovery.img" | Out-Null
	$t = 0
	while ($t -lt 90 -and -not ((adb devices) -match 'recovery')) { Start-Sleep 2; $t += 2 }
}
adb shell 'dd if=/dev/block/bootdevice/by-name/cache of=/tmp/shimlog.bin bs=512 count=128 2>/dev/null'
adb pull /tmp/shimlog.bin $out | Out-Null
if (-not (Test-Path $out)) { 'could not pull log'; exit 1 }
$b = [IO.File]::ReadAllBytes($out)
$magic = [Text.Encoding]::ASCII.GetString($b, 0, 8)
if ($magic -ne 'MIDOSHIM') { "no shim log (magic='$magic')"; exit 1 }
$len = [BitConverter]::ToUInt32($b, 8)
$text = [Text.Encoding]::ASCII.GetString($b, 16, [Math]::Min($len, $b.Length - 16))
$text | Out-File "$root\shim\out\shimlog.txt" -Encoding utf8
$text
