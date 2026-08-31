#!/usr/bin/env bash
# Record a garden suite: one take per garden with its own sound profile,
# then concatenate everything with crossfades on a paper-colored canvas.
# Usage: scripts/suite.sh [seconds_per_garden]
set -euo pipefail
cd "$(dirname "$0")/.."

LEN="${1:-60}"
STAMP=$(date +%Y%m%d-%H%M%S)
CANVAS=1440
XFADE=1.5
PAPER=0xF4F1EA

# scene  profile  tps
GARDENS=(
  "wild-mature.sim wild 500"
  "rain-mature.sim rain 400"
  "islands-mature.sim islands 400"
  "colosseum2-mature.sim colosseum 400"
)

CLIPS=()
for entry in "${GARDENS[@]}"; do
  read -r scene profile tps <<< "$entry"
  echo "=== ${scene} (${profile}, ${tps} TPS) ==="
  bash scripts/alien-ctl.sh start "$scene" "$tps" > /dev/null
  sleep 8
  bash scripts/take.sh "$LEN" "suite-${profile}" 0 12000 12001 "$profile" > "/tmp/suite-${profile}.log" 2>&1
  CLIP=$(ls "takes/suite-${profile}"-*.mp4 | tail -1)
  CLIPS+=("$CLIP")
  echo "clip: $CLIP"
done

echo "=== concatenating ${#CLIPS[@]} clips ==="
INPUTS=()
NORM=""
for i in "${!CLIPS[@]}"; do
  INPUTS+=(-i "${CLIPS[$i]}")
  NORM+="[${i}:v]scale=${CANVAS}:${CANVAS}:force_original_aspect_ratio=decrease,pad=${CANVAS}:${CANVAS}:(ow-iw)/2:(oh-ih)/2:color=${PAPER},setsar=1,fps=30,format=yuv420p[v${i}];"
done

FILTER="$NORM"
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
