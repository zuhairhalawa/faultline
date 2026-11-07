#!/usr/bin/env bash
# Rebuilds, redeploys, restarts Faultline on the target, then runs every fault once
# and prints the event log. Used during development; the dashboard's campaign
# runner does the same thing with pass/fail limits.
T="$(cd "$(dirname "$0")" && pwd)"
bash "$T/build-target.sh" | tail -1 || exit 1
bash "$T/vm.sh" deploy || exit 1
bash "$T/vm.sh" run
inj() { echo "-- $1"; bash "$T/vm.sh" ssh "echo '$1' > /dev/faultline/inject" || echo "   (rejected)"; sleep "${2:-1.5}"; }
inj "crash motor-drv"
inj "hang flow-sensor"
inj "hang motor-drv"
inj "crash pump-ctrl"
inj "crash flow-sensor"
inj "flood 30 200"
inj "flood 41 80"
inj "slow 30 800"
inj "slow 80 600"
inj "stuck 600" 2
bash "$T/events.sh"
