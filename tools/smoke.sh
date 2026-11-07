#!/usr/bin/env bash
# Deploys to the running target, starts Faultline, and runs a first crash and hang test.
V="$(cd "$(dirname "$0")" && pwd)/vm.sh"
bash "$V" deploy || exit 1
bash "$V" ssh 'slay -f fl-supervisor fl-plant flow-sensor motor-drv pump-ctrl fl-inject fl-bridge >/dev/null 2>&1; rm -f /dev/shmem/fl-state; cd /data/fl && (nohup bin/fl-supervisor /data/fl/system.conf /data/fl/bin > /tmp/fl.log 2>&1 &) ; sleep 2; cat /tmp/fl.log; pidin -p fl-plant -p flow-sensor -p motor-drv -p pump-ctrl -p fl-inject -p fl-bridge -p fl-supervisor -F "%a %b %N %p %J" 2>/dev/null | head -20'
echo "== motor"
bash "$V" ssh 'cat /dev/fl/motor; cat /dev/faultline/inject'
echo "== crash motor-drv"
bash "$V" ssh 'echo "crash motor-drv" > /dev/faultline/inject; sleep 1; cat /dev/fl/motor'
echo "== hang flow-sensor"
bash "$V" ssh 'echo "hang flow-sensor" > /dev/faultline/inject; sleep 1'
echo "== bridge events"
bash "$V" ssh 'cat /tmp/fl.log'
