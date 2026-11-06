# Run on OrangeFox (adb shell sh): read-only probe for touch/keys.
# NEVER read /sys/kernel/debug/gpio on MSM8953: it dumps secure GPIOs -> XPU violation -> reset.
mount -t debugfs none /sys/kernel/debug 2>/dev/null
P=$(ls -d /sys/kernel/debug/pinctrl/*1000000* 2>/dev/null | head -1)
echo "== pinctrl dir: $P"
echo "== pinmux-pins"; for p in 6 7 10 11 64 65 85; do grep -E "^pin $p " $P/pinmux-pins 2>/dev/null; done
echo "== functions (index order)"; grep -n "" $P/pinmux-functions 2>/dev/null | grep -E "blsp_i2c3|: function: gpio," | head -4
echo "== groups of blsp_i2c3"; grep -A1 "blsp_i2c3" $P/pinmux-functions 2>/dev/null | head -3
echo "== regulators"; for d in /sys/class/regulator/regulator.*; do n=$(cat $d/name 2>/dev/null); case $n in pm8953_l6|pm8953_l10) echo "$n state=$(cat $d/state) uV=$(cat $d/microvolts) users=$(cat $d/num_users)";; esac; done
echo "== input"; grep -E "^N:" /proc/bus/input/devices
echo "== i2c"; ls /sys/bus/i2c/devices/; for f in /sys/bus/i2c/devices/*-0038/name; do echo "$f: $(cat $f)"; done 2>/dev/null
echo "== clk"; for c in gcc_blsp1_qup3_i2c_apps_clk gcc_blsp1_ahb_clk blsp1_qup3_i2c_apps_clk_src; do echo "$c rate=$(cat /sys/kernel/debug/clk/$c/clk_rate 2>/dev/null) en=$(cat /sys/kernel/debug/clk/$c/clk_enable_count 2>/dev/null)"; done
echo DONE
