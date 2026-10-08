#!/usr/bin/env bash
# Prepare the four iPhone HDR clips on a machine with FFmpeg; leave the originals untouched.
# Usage: tools/prepare_iphone_videos.sh [output directory]
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$project_dir"
output_dir="${1:-video/compatible}"
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd -P)"
if [[ "$output_dir" == "$project_dir/video" ]]; then
    echo "ERROR: output directory must differ from video/ (the originals)." >&2
    exit 2
fi

# FFmpeg applies the rotation metadata before filtering. Keep the original resolution and HLG
# transfer function: only change the codec/bit depth, without changing the pipeline's exposure.
# This works with the already-installed FFmpeg; it requires no optional tone-mapping libraries.
filters='format=yuv420p,sidedata=delete'
temporary=""
trap 'if [[ -n "$temporary" ]]; then rm -f "$temporary"; fi' EXIT
for clip in IMG_5666 IMG_5667 IMG_5668 IMG_5669; do
    echo "Preparing $clip.mp4"
    temporary="$output_dir/.$clip.tmp.mp4"
    ffmpeg -hide_banner -loglevel error -nostdin -y -i "video/$clip.mp4" \
        -map 0:v:0 -an -vf "$filters" -c:v libx264 -preset fast -crf 18 \
        -color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc \
        -map_metadata -1 -metadata:s:v:0 rotate=0 -movflags +faststart "$temporary"
    mv -f "$temporary" "$output_dir/$clip.mp4"
    temporary=""
done
cp video/parking.mp4 "$output_dir/parking.mp4"
echo "Prepared clips: $output_dir"
