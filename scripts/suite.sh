#!/usr/bin/env bash
# Record a garden suite: one take per garden with its own sound profile,
# loudness-normalize each movement, then concatenate with slow crossfades
# on a paper-colored canvas.
# Usage: scripts/suite.sh
# Movement list (scene profile tps length cataclysm_pulses) is edited below —
# the suite is a composition, not a batch job.
set -euo pipefail
cd "$(dirname "$0")/.."

STAMP=$(date +%Y%m%d-%H%M%S)
# record on a second server slot so the long-running garden keeps growing
export ALIEN_SESSION=alien-suite
export ALIEN_OSC_PORT=12040
export ALIEN_GEOM_PORT=12041
CANVAS=1440
XFADE=3.0
PAPER=0xF4F1EA
LUFS=-18

# The arc: quiet falling rain -> polyphonic wooden archipelago ->
# the wild in full life -> the colosseum's hunt as finale.
GARDENS=(
  "rain-mature.sim rain 400 45 0"
  "islands-mature.sim islands 400 55 0"
  "wild-mature.sim wild 500 65 0"
  "colosseum2-mature.sim colosseum 400 70 2"
)

CLIPS=()
for entry in "${GARDENS[@]}"; do
  read -r scene profile tps len pulses <<< "$entry"
  echo "=== ${scene} (${profile}, ${tps} TPS, ${len}s, pulses ${pulses}) ==="
  bash scripts/alien-ctl.sh start "$scene" "$tps" > /dev/null
  sleep 8
  bash scripts/take.sh "$len" "suite-${profile}" "$pulses" "$ALIEN_OSC_PORT" "$ALIEN_GEOM_PORT" "$profile" > "/tmp/suite-${profile}.log" 2>&1
  RAW=$(ls "takes/suite-${profile}"-*.mp4 | tail -1)
  NORM="takes/norm-${profile}-${STAMP}.mp4"
  ffmpeg -y -loglevel error -i "$RAW" -c:v copy -af "loudnorm=I=${LUFS}:TP=-1.5:LRA=11" -c:a aac -b:a 192k "$NORM"
  CLIPS+=("$NORM")
  echo "movement: $NORM"
done

echo "=== concatenating ${#CLIPS[@]} movements ==="
INPUTS=()
NORMF=""
for i in "${!CLIPS[@]}"; do
  INPUTS+=(-i "${CLIPS[$i]}")
  NORMF+="[${i}:v]scale=${CANVAS}:${CANVAS}:force_original_aspect_ratio=decrease,pad=${CANVAS}:${CANVAS}:(ow-iw)/2:(oh-ih)/2:color=${PAPER},setsar=1,fps=30,format=yuv420p[v${i}];"
done

FILTER="$NORMF"
PREV_V="v0"
PREV_A="0:a"
OFFSET=0
for i in $(seq 1 $((${#CLIPS[@]} - 1))); do
  DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "${CLIPS[$((i-1))]}")
  OFFSET=$(python3 -c "print(round(${OFFSET} + ${DUR} - ${XFADE}, 3))")
  FILTER+="[${PREV_V}][v${i}]xfade=transition=fade:duration=${XFADE}:offset=${OFFSET}[xv${i}];"
  FILTER+="[${PREV_A}][${i}:a]acrossfade=d=${XFADE}[xa${i}];"
  PREV_V="xv${i}"
  PREV_A="xa${i}"
done

OUT="takes/suite-${STAMP}.mp4"
ffmpeg -y -loglevel error "${INPUTS[@]}" -filter_complex "$FILTER" \
  -map "[${PREV_V}]" -map "[${PREV_A}]" \
  -c:v libx264 -preset medium -crf 23 -pix_fmt yuv420p -c:a aac -b:a 192k "$OUT"
echo "suite written: $OUT"
ffprobe -v error -show_entries format=duration -of csv "$OUT"
