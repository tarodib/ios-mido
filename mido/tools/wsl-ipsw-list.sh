#!/bin/bash
set -euo pipefail
cd ~/fw/ipsw-10,5
I=iPhone_5.5_P3_16.7.16_20H392_Restore.ipsw
unzip -l "$I" | sort -k1 -n -r | head -12
unzip -o -q "$I" BuildManifest.plist 'Firmware/all_flash/DeviceTree.d211ap.im4p' -d x
python3 - <<'PY'
import plistlib
m = plistlib.load(open('x/BuildManifest.plist','rb'))
for bi in m['BuildIdentities']:
    info = bi['Info']
    if info.get('DeviceClass') != 'd211ap' or 'Erase' not in info.get('Variant',''):
        continue
    print(info['Variant'])
    for k in ('OS', 'SystemVolume', 'Cryptex1,SystemOS', 'Cryptex1,AppOS', 'RestoreRamDisk'):
        if k in bi['Manifest']:
            print(' ', k, bi['Manifest'][k]['Info']['Path'])
    break
PY
ls -la x/Firmware/all_flash/
