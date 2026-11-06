#!/sbin/sh
# Run on the phone in OrangeFox: which ways exist to read physical RAM?
ls -l /proc/config.gz /proc/kcore /dev/kmem /dev/mem 2>&1
uname -r
zcat /proc/config.gz 2>/dev/null | grep -E 'CONFIG_DEVMEM|CONFIG_STRICT_DEVMEM|CONFIG_MODULES|CONFIG_PSTORE_RAM|CONFIG_KEXEC|CONFIG_MODVERSIONS'
