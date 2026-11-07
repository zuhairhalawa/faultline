#!/usr/bin/env bash
# Freezes the flow sensor and shows what state the controller thread is blocked in.
V="$(cd "$(dirname "$0")" && pwd)/vm.sh"
bash "$V" ssh 'echo "hang flow-sensor" > /dev/faultline/inject; sleep 0.03; pidin -p pump-ctrl -p flow-sensor -F "%a %b %N %p %J %B"; sleep 0.03; pidin -p pump-ctrl -F "%a %b %N %p %J %B"'
