#!/usr/bin/env bash
# Builds (does not run) a QEMU x86_64 QNX 8.0 image in ~/fl-vm with SSH key login for root.
set -euo pipefail
source "${QNX_SDP:-$HOME/qnx800}/qnxsdp-env.sh" > /dev/null
export COLUMNS=120
if [ ! -f "$HOME/.ssh/id_ed25519.pub" ]; then
    echo "No SSH key at ~/.ssh/id_ed25519; creating one (no passphrase) for root login to the VM."
    mkdir -p "$HOME/.ssh"
    ssh-keygen -q -t ed25519 -N '' -f "$HOME/.ssh/id_ed25519"
fi
VM="$HOME/fl-vm"
mkdir -p "$VM"
cd "$VM"
mkqnximage --type=qemu --arch=x86_64 --noprompt --ssh-ident="$HOME/.ssh/id_ed25519.pub" \
    --hostname=faultline --ram=2G --cpu=4 "$@" 2>&1 | tail -25
ls -l output 2>/dev/null | head
