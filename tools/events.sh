#!/usr/bin/env bash
# Prints the event lines the target's bridge replays on connect, plus one state line.
IP=$(cat "$HOME/fl-vm/.ip")
exec 3<>"/dev/tcp/$IP/7700" || exit 1
timeout 1.5 cat <&3 > /tmp/fl-stream.txt
grep '"kind":"event"' /tmp/fl-stream.txt
echo "== last state"
grep '"kind":"state"' /tmp/fl-stream.txt | tail -1
