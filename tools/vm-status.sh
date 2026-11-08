#!/usr/bin/env bash
# Read-only status of the QNX QEMU target.
source "${QNX_SDP:-$HOME/qnx800}/qnxsdp-env.sh" > /dev/null
cd "$HOME/fl-vm"
pgrep -a qemu-system | cut -c1-200
echo "getip: $(MKQNXIMAGE_NOPROMPT=1 mkqnximage --getip 2>&1 | tail -3)"
ip neigh show dev virbr0 2>/dev/null
cat /var/lib/libvirt/dnsmasq/virbr0.status 2>/dev/null | head -20
