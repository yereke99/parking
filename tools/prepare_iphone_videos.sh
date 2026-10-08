#!/usr/bin/env bash
# Prepare every clip in video/ for the Jetson on a machine with FFmpeg; leave the originals
# untouched. Clips that are already 8-bit H.264 are copied; anything else (the iPhone 10-bit HEVC
# HDR clips) is re-encoded. Every video file in video/ is handled, however many there are.
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
shopt -s nullglob nocaseglob
clips=(video/*.mp4 video/*.mov video/*.m4v video/*.mkv video/*.avi)
shopt -u nullglob nocaseglob
if (( ${#clips[@]} == 0 )); then
    echo "ERROR: no video files in video/." >&2
    exit 2
fi
for source in "${clips[@]}"; do
    clip="$(basename "${source%.*}")"
    codec="$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,pix_fmt \
        -of csv=p=0 "$source" 2>/dev/null || true)"
    if [[ "$codec" == "h264,yuv420p" ]]; then
        echo "Copying $(basename "$source") (already 8-bit H.264)"
        cp "$source" "$output_dir/$clip.mp4"
        continue
    fi
    echo "Preparing $(basename "$source") (${codec:-unknown codec})"
    temporary="$output_dir/.$clip.tmp.mp4"
    ffmpeg -hide_banner -loglevel error -nostdin -y -i "$source" \
        -map 0:v:0 -an -vf "$filters" -c:v libx264 -preset fast -crf 18 \
        -color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc \
        -map_metadata -1 -metadata:s:v:0 rotate=0 -movflags +faststart "$temporary"
    mv -f "$temporary" "$output_dir/$clip.mp4"
    temporary=""
done
echo "Prepared clips: $output_dir"
