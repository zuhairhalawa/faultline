#!/usr/bin/env bash
# Controls the QNX QEMU target in ~/fl-vm.
#   tools/vm.sh start     boot headless (in the background)
#   tools/vm.sh ip        print its IP address
#   tools/vm.sh stop
#   tools/vm.sh ssh CMD   run a command on the target as root
#   tools/vm.sh deploy    copy the built programs and config to /data/fl on the target
#   tools/vm.sh run       start Faultline on the target (restarts it if running)
#   tools/vm.sh halt      stop Faultline on the target
set -uo pipefail
source "$HOME/qnx800/qnxsdp-env.sh" > /dev/null
export COLUMNS=120 MKQNXIMAGE_NOPROMPT=1
VM="$HOME/fl-vm"
BUILD="${FL_BUILD_DIR:-$HOME/faultline-build}/target"
SSHOPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5)
cd "$VM"

vm_ip() {
    if [ -f "$VM/.ip" ]; then cat "$VM/.ip"; return; fi
    mkqnximage --getip 2>/dev/null | grep -Eo '([0-9]+\.){3}[0-9]+' | head -1
}

case "${1:-}" in
start)
    rm -f "$VM/.ip"
    mkqnximage --noprompt --run=-h 2>&1 | tail -5
    for i in $(seq 1 60); do
        ip=$(mkqnximage --getip 2>/dev/null | grep -Eo '([0-9]+\.){3}[0-9]+' | head -1)
        if [ -n "$ip" ] && ssh "${SSHOPTS[@]}" "root@$ip" true 2>/dev/null; then
            echo "$ip" > "$VM/.ip"
            echo "TARGET UP at $ip after ${i}s"
            exit 0
        fi
        sleep 1
    done
    echo "TARGET NOT REACHABLE"; exit 1
    ;;
ip) vm_ip ;;
stop) mkqnximage --stop; rm -f "$VM/.ip" ;;
ssh) shift; ssh "${SSHOPTS[@]}" "root@$(vm_ip)" "$@" ;;
halt)
    # Stop Faultline on the target. The supervisor goes first, or it would restart the rest.
    ssh "${SSHOPTS[@]}" "root@$(vm_ip)" 'slay -f fl-supervisor >/dev/null 2>&1; sleep 0.3; slay -f fl-plant flow-sensor motor-drv pump-ctrl fl-inject fl-bridge >/dev/null 2>&1; sleep 0.5; rm -f /dev/shmem/fl-state'
    ;;
run)
    # Start Faultline on the target (stopping any running copy first).
    "$0" halt
    ssh "${SSHOPTS[@]}" "root@$(vm_ip)" 'cd /data/fl && (nohup bin/fl-supervisor /data/fl/system.conf /data/fl/bin > /tmp/fl.log 2>&1 &); sleep 1.5; cat /tmp/fl.log'
    ;;
deploy)
    ip=$(vm_ip)
    "$0" halt
    ssh "${SSHOPTS[@]}" "root@$ip" 'mkdir -p /data/fl/bin' &&
    scp "${SSHOPTS[@]}" -q "$BUILD"/build/x86_64/* "root@$ip:/data/fl/bin/" &&
    scp "${SSHOPTS[@]}" -q "$BUILD/system.conf" "root@$ip:/data/fl/" &&
    echo "DEPLOYED to $ip:/data/fl"
    ;;
*) echo "usage: vm.sh start|stop|ip|deploy|run|halt|ssh CMD"; exit 2 ;;
esac
