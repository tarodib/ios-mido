#!/bin/bash
# Download the full latest iPhone10,5 (iPhone 8 Plus, D211AP) IPSW into ~/fw/ipsw-10,5
set -euo pipefail
export PATH=$HOME/tools/bin:$PATH
mkdir -p ~/fw/ipsw-10,5
cd ~/fw/ipsw-10,5
ipsw download ipsw --device iPhone10,5 --latest --confirm -y 2>&1 | tail -20
ls -la
