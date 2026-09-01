#!/usr/bin/env bash
# Record an audio+video take of whatever the alien server is currently running.
# Usage: scripts/take.sh [seconds] [label] [cataclysm_pulses]
#   seconds           recording length (default 60)
#   label             output name label (default "take")
#   cataclysm_pulses  if > 0, fire pulses at 1/3 and 2/3 of the take (default 0)
set -euo pipefail
cd "$(dirname "$0")/.."

SECONDS_LEN="${1:-60}"
LABEL="${2:-take}"
PULSES="${3:-0}"
OSC_PORT="${4:-12000}"
GEOM_PORT="${5:-12001}"
SERVER_IP="${ALIEN_IP:-100.70.183.86}"
PROFILE="${6:-default}"
QUIET="${7:-1}"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="takes/${LABEL}-${STAMP}.mp4"
mkdir -p takes

set +e
pkill -f sclang 2>/dev/null
pkill -f scsynth 2>/dev/null
set -e
sleep 2
rm -f sound/test-capture.aiff

echo "audio: starting sclang (records ${SECONDS_LEN}s after 15s warmup)"
nohup /Applications/SuperCollider.app/Contents/MacOS/sclang sound/test-capture.scd "$SECONDS_LEN" "$OSC_PORT" "$SERVER_IP" "$PROFILE" "$QUIET" > /tmp/take-sclang.log 2>&1 &

sleep 13
echo "video: starting renderer"
(viz/venv/bin/python viz/alien_viz.py --look ink --offscreen --server "$SERVER_IP" --port "$GEOM_PORT" --record /tmp/take-video.mp4 \
    --exit-after $((SECONDS_LEN + 6)) > /tmp/take-viz.log 2>&1 &)

if [ "$PULSES" -gt 0 ]; then
  (sleep $((SECONDS_LEN / 3 + 15)); bash scripts/alien-ctl.sh cataclysm "$PULSES" >/dev/null) &
  (sleep $((SECONDS_LEN * 2 / 3 + 15)); bash scripts/alien-ctl.sh cataclysm "$PULSES" >/dev/null) &
fi

sleep $((SECONDS_LEN + 12))
# wait until the renderer has finalized the video (moov atom written on exit)
for _ in $(seq 1 40); do
  pgrep -f "alien_viz.*take-video" > /dev/null || break
  sleep 2
done
sleep 1

if [ ! -f sound/test-capture.aiff ]; then
  echo "ERROR: no audio recorded (see /tmp/take-sclang.log)" >&2
  exit 1
fi
ffmpeg -y -loglevel error -i /tmp/take-video.mp4 -i sound/test-capture.aiff \
  -c:v copy -c:a aac -b:a 192k -shortest "$OUT"
echo "take written: $OUT"
ffprobe -v error -show_entries format=duration -of csv "$OUT"
