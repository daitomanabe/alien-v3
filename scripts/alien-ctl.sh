#!/usr/bin/env bash
# CLI control for the headless ALIEN server on raytrek4090.
# Usage:
#   scripts/alien-ctl.sh start [scene] [tps]   start server in tmux (default: hanging-garden.sim, 200 TPS)
#   scripts/alien-ctl.sh stop                  stop server
#   scripts/alien-ctl.sh status                tmux state + recent log
#   scripts/alien-ctl.sh log [n]               last n log lines (default 20)
#   scripts/alien-ctl.sh scenes                list remote scene files
#   scripts/alien-ctl.sh cataclysm [n]         trigger n cataclysm pulses (default 1) — provokes attacks/detonations
set -euo pipefail

HOST=raytrek4090
REMOTE_DIR='~/workspaces/alien-v3'
SESSION=alien-srv
SERVER_IP=100.87.26.1
OSC_PORT=12000

cmd="${1:-status}"

case "$cmd" in
  start)
    scene="${2:-hanging-garden.sim}"
    tps="${3:-200}"
    ssh "$HOST" "tmux kill-session -t $SESSION 2>/dev/null; tmux new -d -s $SESSION \
      \"cd $REMOTE_DIR/build-ninja/Release && ./alien_server -i ../../scenes/$scene --rate 20 --tps $tps 2>&1 | tee /tmp/alien-srv.log\""
    sleep 3
    ssh "$HOST" 'head -3 /tmp/alien-srv.log'
    ;;
  stop)
    ssh "$HOST" "tmux kill-session -t $SESSION 2>/dev/null && echo stopped || echo not-running"
    ;;
  status)
    ssh "$HOST" "tmux has-session -t $SESSION 2>/dev/null && echo RUNNING || echo STOPPED; tail -3 /tmp/alien-srv.log 2>/dev/null"
    ;;
  log)
    ssh "$HOST" "tail -${2:-20} /tmp/alien-srv.log"
    ;;
  scenes)
    ssh "$HOST" "ls -la $REMOTE_DIR/scenes/"
    ;;
  cataclysm)
    count="${2:-1}"
    python3 - "$SERVER_IP" "$OSC_PORT" "$count" <<'EOF'
import socket, sys, time
host, port, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
msg = b"/alien/cataclysm\x00\x00\x00\x00,\x00\x00\x00"
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for i in range(count):
    sock.sendto(msg, (host, port))
    time.sleep(0.3)
print(f"sent {count} cataclysm pulse(s)")
EOF
    ;;
  *)
    echo "unknown command: $cmd" >&2
    exit 1
    ;;
esac
