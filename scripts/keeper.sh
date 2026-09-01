#!/usr/bin/env bash
# Hourly snapshots of the running gardens into archive/, one OSC save port per
# garden. The server writes saved-<timestep>.sim into its cwd with no path
# argument, so save each garden sequentially and prefix the file on the move.
# Usage: keeper.sh [name:port ...]   (default: m2:12000 m3:12060)
GARDENS=("${@:-m2:12000 m3:12060}")
[ $# -eq 0 ] && GARDENS=(m2:12000 m3:12060)
cd ~/workspaces/alien-v3/build-ninja/Release || exit 1
while true; do
  sleep 3600
  for entry in "${GARDENS[@]}"; do
    name="${entry%%:*}"
    port="${entry##*:}"
    python3 - "$port" <<'PYEOF'
import socket, sys
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.sendto(b"/alien/save\x00,\x00\x00\x00", ("127.0.0.1", int(sys.argv[1])))
PYEOF
    sleep 8
    for f in saved-*.sim; do
      [ -e "$f" ] && mv "$f" ~/workspaces/alien-v3/archive/"${name}-${f}"
    done
  done
done
