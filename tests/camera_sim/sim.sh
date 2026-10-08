#!/usr/bin/env bash
# SIMULATED Hikvision camera LAN in Docker, for developing and testing kz_anpr camera mode on a
# development machine (Docker Desktop on a Mac works). Development tooling only: it never proves
# real-hardware behaviour (see README.md in this directory for what it cannot simulate).
#
# Each simulated camera is two containers sharing one network namespace on the bridge network
# "kzcam" (192.168.77.0/24): MediaMTX serving RTSP (digest auth, TCP, a looping test clip) and
# fake_hikvision.py answering SADP, WS-Discovery, ISAPI, ONVIF and the SDK port.
#
#   sim.sh up [N]           start cameras 1..N (default 4; 5 adds an un-activated camera)
#   sim.sh down             remove all simulator containers and the network
#   sim.sh status           one line per camera
#   sim.sh stop-camera K    power loss / reboot: stop camera K's two containers
#   sim.sh start-camera K   power back: start them again
#   sim.sh close-rtsp K     camera K stays on the network but nothing listens on its RTSP port
#   sim.sh open-rtsp K      undo close-rtsp
#   sim.sh trigger-anpr K   camera K reports its ANPR plate now (alertStream)
#   sim.sh logs K [rtsp|hik]
#   sim.sh run [CMD ...]    run CMD (default: a shell) at 192.168.77.5 on kzcam in the dev image,
#                           repository at /src (read-only), build dir at /b, sim.env as env-file;
#                           SIM_UPLINK=1 adds a second, NAT network as a stand-in for the GSM uplink
#   sim.sh build [TARGET]   build TARGET (default kz_anpr) into the build dir with that container
#   sim.sh verify           end-to-end checks of the simulator itself
#   sim.sh clean            down, then delete the generated clips and state (.cache)
set -euo pipefail

SIM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SIM_DIR/../.." && pwd)"
CACHE_DIR="$SIM_DIR/.cache"
CLIPS_DIR="$CACHE_DIR/clips"
ENV_FILE="$SIM_DIR/sim.env"
BUILD_DIR="${SIM_BUILD_DIR:-$CACHE_DIR/build-linux}"
WORK_DIR="$CACHE_DIR/work"

NETWORK="kzcam"
UPLINK_NETWORK="kzcam-uplink"
SUBNET="192.168.77.0/24"
GATEWAY="192.168.77.1"
CLIENT_IP="192.168.77.5"
VERIFY_IP="192.168.77.6"
LOCKOUT_IP="192.168.77.7"
MEDIAMTX_IMAGE="${SIM_MEDIAMTX_IMAGE:-bluenviron/mediamtx:1.21.1-ffmpeg}"
DEV_IMAGE="${SIM_DEV_IMAGE:-kz-anpr-dev:focal-gcc7}"
SOURCE_CLIP="$REPO_DIR/video/parking.mp4"
LABEL="org.kz-anpr.camera-sim"
MAX_CAMERAS=5
DEFAULT_CAMERAS=4
# Bump when the clip recipe below changes so cached transcodes are rebuilt.
MEDIA_RECIPE="v1 x265-crf23-gop60-nobf sub304x540 pictures@7.6s"

log() { printf 'sim: %s\n' "$*" >&2; }
die() { printf 'sim: error: %s\n' "$*" >&2; exit 1; }

numbers() {
    # FROM..TO, nothing when FROM > TO (BSD seq counts down instead). Bash 3.2 compatible, as
    # is the whole script: macOS ships bash 3.2.
    local i="$1"
    while [[ "$i" -le "$2" ]]; do
        printf '%s\n' "$i"
        i=$((i + 1))
    done
}

# ---- configuration ------------------------------------------------------------------------------

env_value() {
    # KEY from sim.env (docker --env-file format: KEY=value, no quotes).
    local line
    while IFS= read -r line || [[ -n "$line" ]]; do
        case "$line" in
            "$1="*) printf '%s' "${line#*=}"; return 0 ;;
        esac
    done <"$ENV_FILE"
    return 1
}

load_env() {
    [[ -f "$ENV_FILE" ]] || die "missing $ENV_FILE"
    SIM_USERNAME="$(env_value HIKVISION_USERNAME)" || die "HIKVISION_USERNAME missing in sim.env"
    SIM_PASSWORD="$(env_value HIKVISION_PASSWORD)" || die "HIKVISION_PASSWORD missing in sim.env"
    SIM_ONVIF_USERNAME="$(env_value ONVIF_USERNAME)" || die "ONVIF_USERNAME missing in sim.env"
    SIM_ONVIF_PASSWORD="$(env_value ONVIF_PASSWORD)" || die "ONVIF_PASSWORD missing in sim.env"
    SIM_CAM4_PASSWORD="$(env_value SIM_CAM4_PASSWORD)" || die "SIM_CAM4_PASSWORD missing in sim.env"
}

camera_spec() {
    # Sets the CAM_* variables of simulated camera $1. The default set covers the scenarios
    # camera mode must handle: H.264, H.265 + ONVIF, a non-default RTSP port, a password that
    # differs from the shared one, and (cam5, optional) a camera that is not activated.
    local k="$1"
    case "$k" in
        [1-5]) ;;
        *) die "camera number must be 1..$MAX_CAMERAS, got '$k'" ;;
    esac
    CAM="cam$k"
    CAM_IP="192.168.77.2$k"
    CAM_MAC="bc:ad:28:77:00:2$k"
    CAM_CODEC="h264"
    CAM_RTSP_PORT=554
    CAM_ONVIF="off"
    CAM_ACTIVATED="true"
    CAM_PASSWORD="$SIM_PASSWORD"
    CAM_NOTE="shared login"
    case "$k" in
        2) CAM_CODEC="h265"; CAM_ONVIF="on"; CAM_NOTE="H.265, ONVIF on" ;;
        3) CAM_RTSP_PORT=8554; CAM_NOTE="RTSP on 8554" ;;
        4) CAM_PASSWORD="$SIM_CAM4_PASSWORD"; CAM_NOTE="other password (RTSP_AUTH_FAILED)" ;;
        5) CAM_ACTIVATED="false"; CAM_NOTE="not activated (CAMERA_NOT_ACTIVATED)" ;;
    esac
    RTSP_CONTAINER="kzcam-$CAM-rtsp"
    HIK_CONTAINER="kzcam-$CAM-hik"
    STATE_DIR="$CACHE_DIR/cams/$CAM"
}

# ---- docker helpers -----------------------------------------------------------------------------

require_docker() {
    command -v docker >/dev/null 2>&1 || die "docker not found"
    docker info >/dev/null 2>&1 || die "the Docker daemon is not running"
}

require_images() {
    if ! docker image inspect "$MEDIAMTX_IMAGE" >/dev/null 2>&1; then
        log "pulling $MEDIAMTX_IMAGE"
        docker pull "$MEDIAMTX_IMAGE" >/dev/null
    fi
    docker image inspect "$DEV_IMAGE" >/dev/null 2>&1 ||
        die "image $DEV_IMAGE not found (build the kz-anpr dev image first, or set SIM_DEV_IMAGE)"
}

container_state() {
    # docker inspect prints an empty line for a missing container, so test its status.
    local state
    if state="$(docker inspect -f '{{.State.Status}}' "$1" 2>/dev/null)"; then
        printf '%s' "$state"
    else
        printf 'absent'
    fi
}

container_label() {
    local value
    if value="$(docker inspect -f "{{index .Config.Labels \"$2\"}}" "$1" 2>/dev/null)"; then
        printf '%s' "$value"
    fi
}

ensure_network() {
    # A plain bridge. Not --internal: Docker's isolation rules for internal networks drop every
    # packet addressed outside the subnet, multicast discovery (239.255.255.250) included. The
    # price is a default route via 192.168.77.1 in every container on it; SIM_UPLINK=1 moves
    # the client's default route to another network, as on the Jetson.
    local current
    if current="$(docker network inspect -f \
        '{{range .IPAM.Config}}{{.Subnet}}{{end}} {{.Internal}}' "$NETWORK" 2>/dev/null)"; then
        [[ "$current" == "$SUBNET false" ]] ||
            die "network $NETWORK exists as '$current', expected '$SUBNET false' (sim.sh down)"
        return 0
    fi
    log "creating network $NETWORK ($SUBNET)"
    docker network create --driver bridge --subnet "$SUBNET" --gateway "$GATEWAY" \
        --label "$LABEL=1" "$NETWORK" >/dev/null
}

ensure_uplink_network() {
    # A NAT network that gets the client's default route (gw-priority 1), standing in for the
    # GSM modem; its subnet is chosen by Docker, never the camera subnet.
    if ! docker network inspect "$UPLINK_NETWORK" >/dev/null 2>&1; then
        log "creating network $UPLINK_NETWORK (uplink stand-in)"
        docker network create --driver bridge --label "$LABEL=1" "$UPLINK_NETWORK" >/dev/null
    fi
}

# ---- media --------------------------------------------------------------------------------------

prepare_media() {
    # H.265 main stream, sub streams and ANPR pictures from video/parking.mp4 (the clip whose
    # expected plate is 152JTA02), plus their measured parameters for ISAPI. Cached until the
    # clip or the recipe changes.
    [[ -f "$SOURCE_CLIP" ]] || die "missing $SOURCE_CLIP"
    mkdir -p "$CLIPS_DIR"
    local stamp
    stamp="$(cksum <"$SOURCE_CLIP") $MEDIA_RECIPE"
    if [[ -f "$CLIPS_DIR/.stamp" && "$(cat "$CLIPS_DIR/.stamp")" == "$stamp" &&
        -s "$CLIPS_DIR/clips.info" ]]; then
        return 0
    fi
    log "transcoding test clips into $CLIPS_DIR (first run only, about a minute)"
    rm -f "$CLIPS_DIR/.stamp"
    docker run --rm --entrypoint sh -v "$REPO_DIR/video:/video:ro" -v "$CLIPS_DIR:/clips" \
        "$MEDIAMTX_IMAGE" -c '
set -eu
src=/video/parking.mp4
x265="keyint=60:min-keyint=60:bframes=0:repeat-headers=1:log-level=error"
ff() { ffmpeg -hide_banner -loglevel error -nostdin -y "$@"; }
ff -i "$src" -map 0:v:0 -an -c:v libx265 -preset medium -crf 23 -pix_fmt yuv420p \
    -profile:v main -x265-params "$x265" -tag:v hvc1 /clips/.main_h265.mp4
ff -i "$src" -map 0:v:0 -an -vf scale=304:540 -c:v libx265 -preset medium -crf 23 \
    -pix_fmt yuv420p -profile:v main -x265-params "$x265" -tag:v hvc1 /clips/.sub_h265.mp4
ff -i "$src" -map 0:v:0 -an -vf scale=304:540 -c:v libx264 -preset medium -crf 23 \
    -pix_fmt yuv420p -profile:v main -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
    /clips/.sub_h264.mp4
ff -ss 7.6 -i "$src" -frames:v 1 -q:v 3 /clips/.detectionPicture.jpg
ff -ss 7.6 -i "$src" -frames:v 1 -vf crop=117:44:338:338 -q:v 2 /clips/.licensePlatePicture.jpg
for name in main_h265.mp4 sub_h265.mp4 sub_h264.mp4 detectionPicture.jpg \
    licensePlatePicture.jpg; do
    mv "/clips/.$name" "/clips/$name"
done
# name codec width height centi-fps gov(max frames between key frames)
for clip in /video/parking.mp4 /clips/main_h265.mp4 /clips/sub_h265.mp4 /clips/sub_h264.mp4; do
    info=$(ffprobe -v error -select_streams v:0 -show_entries \
        stream=codec_name,width,height,r_frame_rate -of csv=p=0 "$clip")
    gov=$(ffprobe -v error -select_streams v:0 -show_entries packet=flags -of csv=p=0 "$clip" |
        awk "BEGIN { last = -1; gov = 0 } { if (substr(\$0, 1, 1) == \"K\") {
            if (last >= 0 && NR - last > gov) gov = NR - last; last = NR } }
            END { print (gov > 0 ? gov : NR) }")
    echo "$(basename "$clip") $info $gov" | awk -F"[ ,/]" "{ printf \"%s %s %s %s %d %s\\n\",
        \$1, \$2, \$3, \$4, (\$5 * 100) / \$6, \$7 }"
done >/clips/.clips.info
mv /clips/.clips.info /clips/clips.info
'
    printf '%s\n' "$stamp" >"$CLIPS_DIR/.stamp"
}

clip_channel_arg() {
    # "101,H.264,608x1080,3000,160" for the --channel option of fake_hikvision.py.
    local channel="$1" clip="$2" line
    line="$(awk -v n="$clip" '$1 == n' "$CLIPS_DIR/clips.info")"
    [[ -n "$line" ]] || die "no parameters for $clip in $CLIPS_DIR/clips.info"
    printf '%s\n' "$line" | awk -v ch="$channel" '{
        codec = ($2 == "hevc") ? "H.265" : ($2 == "h264") ? "H.264" : toupper($2)
        printf "%s,%s,%sx%s,%s,%s", ch, codec, $3, $4, $5, $6 }'
}

# ---- per-camera configuration -------------------------------------------------------------------

yaml_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/''/g")"
}

mediamtx_config() {
    # $1: "open" (RTSP on the camera's address) or "closed" (RTSP on loopback only, so the
    # publishers keep running while the camera's RTSP port refuses connections).
    local mode="$1" address=":$CAM_RTSP_PORT" main sub loop publish
    # Each stream is a clip looped in real time and copied (no re-encoding) to MediaMTX.
    loop="ffmpeg -hide_banner -loglevel error -nostdin -re -stream_loop -1 -i"
    publish="-map 0:v:0 -c copy -f rtsp -rtsp_transport tcp"
    publish="$publish rtsp://127.0.0.1:\$RTSP_PORT/\$MTX_PATH"
    if [[ "$mode" == "closed" ]]; then
        address="127.0.0.1:$CAM_RTSP_PORT"
    fi
    if [[ "$CAM_CODEC" == "h265" ]]; then
        main="/clips/main_h265.mp4"
        sub="/clips/sub_h265.mp4"
    else
        main="/video/parking.mp4"
        sub="/clips/sub_h264.mp4"
    fi
    cat <<EOF
# Generated by tests/camera_sim/sim.sh for the SIMULATED camera $CAM ($mode). Do not edit.
logLevel: info
logDestinations: [stdout, file]
logFile: /state/mediamtx.log
readTimeout: 10s
writeTimeout: 10s
authMethod: internal
authInternalUsers:
  # Publishing (the looping ffmpeg) from inside the camera only.
  - user: any
    pass:
    ips: ['127.0.0.1', '::1']
    permissions:
      - action: publish
EOF
    if [[ "$CAM_ACTIVATED" == "true" ]]; then
        cat <<EOF
  - user: $(yaml_quote "$SIM_USERNAME")
    pass: $(yaml_quote "$CAM_PASSWORD")
    ips: []
    permissions:
      - action: read
EOF
    fi
    cat <<EOF
api: false
metrics: false
pprof: false
playback: false
rtsp: true
rtspTransports: [tcp]
rtspEncryption: "no"
rtspAddress: $address
# Hikvision cameras use digest; basic is off by default.
rtspAuthMethods: [digest]
rtmp: false
hls: false
webrtc: false
srt: false
moq: false
paths:
  Streaming/Channels/101:
    runOnInit: $loop $main $publish
    runOnInitRestart: yes
  Streaming/Channels/102:
    runOnInit: $loop $sub $publish
    runOnInitRestart: yes
  # Any other path is known but has no stream, so a reader with valid credentials gets
  # 404 (RTSP_STREAM_PATH_INVALID) as from a camera, not 400.
  "~^.*$": {}
EOF
}

hik_arguments() {
    local main_clip sub_clip
    if [[ "$CAM_CODEC" == "h265" ]]; then
        main_clip="main_h265.mp4"
        sub_clip="sub_h265.mp4"
    else
        main_clip="parking.mp4"
        sub_clip="sub_h264.mp4"
    fi
    printf '%s\n' --name "$CAM" --model DS-TCG406-E --ip "$CAM_IP" --mac "$CAM_MAC" \
        --netmask 255.255.255.0 --gateway "$GATEWAY" --http-port 80 --rtsp-port "$CAM_RTSP_PORT" --sdk-port 8000 \
        --activated "$CAM_ACTIVATED" --onvif "$CAM_ONVIF" \
        --channel "$(clip_channel_arg 101 "$main_clip")" \
        --channel "$(clip_channel_arg 102 "$sub_clip")" \
        --anpr-plate 152JTA02 --anpr-interval 20 \
        --anpr-picture /clips/licensePlatePicture.jpg --anpr-picture /clips/detectionPicture.jpg \
        --rtsp-log /state/mediamtx.log
}

write_camera_files() {
    mkdir -p "$STATE_DIR"
    mediamtx_config open >"$STATE_DIR/mediamtx-open.yml"
    mediamtx_config closed >"$STATE_DIR/mediamtx-closed.yml"
    hik_arguments >"$STATE_DIR/fake_hikvision.args"
}

camera_hash() {
    cat "$STATE_DIR/mediamtx-open.yml" "$STATE_DIR/fake_hikvision.args" \
        "$SIM_DIR/fake_hikvision.py" | cksum | awk '{ print $1 }'
}

activate_config() {
    # Hot-swaps MediaMTX's configuration ($1: open|closed) inside the running container: a
    # rename there is what MediaMTX's file watcher sees (host-side writes may not propagate).
    local mode="$1"
    if cmp -s "$STATE_DIR/mediamtx-$mode.yml" "$STATE_DIR/mediamtx.yml"; then
        return 0
    fi
    docker exec "$RTSP_CONTAINER" sh -c "cp /state/mediamtx-$mode.yml /state/.mediamtx.yml.new &&
        mv /state/.mediamtx.yml.new /state/mediamtx.yml"
}

create_camera() {
    local hash="$1" args=()
    local line
    while IFS= read -r line; do
        args+=("$line")
    done <"$STATE_DIR/fake_hikvision.args"
    docker rm -f "$HIK_CONTAINER" "$RTSP_CONTAINER" >/dev/null 2>&1 || true
    cp "$STATE_DIR/mediamtx-open.yml" "$STATE_DIR/mediamtx.yml"
    : >"$STATE_DIR/mediamtx.log"
    docker run -d --name "$RTSP_CONTAINER" --hostname "sim-$CAM" \
        --label "$LABEL=1" --label "$LABEL.camera=$CAM" --label "$LABEL.config=$hash" \
        --network "$NETWORK" --ip "$CAM_IP" --mac-address "$CAM_MAC" \
        -v "$STATE_DIR:/state" -v "$REPO_DIR/video:/video:ro" -v "$CLIPS_DIR:/clips:ro" \
        "$MEDIAMTX_IMAGE" /state/mediamtx.yml >/dev/null
    docker run -d --name "$HIK_CONTAINER" \
        --label "$LABEL=1" --label "$LABEL.camera=$CAM" --label "$LABEL.config=$hash" \
        --network "container:$RTSP_CONTAINER" \
        -e FAKE_HIK_USERNAME="$SIM_USERNAME" -e FAKE_HIK_PASSWORD="$CAM_PASSWORD" \
        -e FAKE_HIK_ONVIF_USERNAME="$SIM_ONVIF_USERNAME" \
        -e FAKE_HIK_ONVIF_PASSWORD="$SIM_ONVIF_PASSWORD" \
        -v "$SIM_DIR:/sim:ro" -v "$STATE_DIR:/state:ro" -v "$CLIPS_DIR:/clips:ro" \
        "$DEV_IMAGE" python3 -u /sim/fake_hikvision.py "${args[@]}" >/dev/null
}

wait_camera_ready() {
    # Both streams published and the web service answering. $1: unix time the camera (or its
    # MediaMTX configuration) was (re)started.
    local since="$1" deadline published http
    deadline=$(($(date +%s) + 45))
    while :; do
        published="$(docker logs --since "$since" "$RTSP_CONTAINER" 2>&1 |
            grep -c "is publishing to path" || true)"
        http="$(docker exec "$HIK_CONTAINER" curl -s -o /dev/null -w '%{http_code}' \
            --max-time 2 http://127.0.0.1:80/ 2>/dev/null || true)"
        if [[ "$published" -ge 2 && ("$http" == "200" || "$http" == "401") ]]; then
            return 0
        fi
        if [[ "$(date +%s)" -ge "$deadline" ]]; then
            docker logs --tail 20 "$RTSP_CONTAINER" >&2 || true
            docker logs --tail 20 "$HIK_CONTAINER" >&2 || true
            die "$CAM did not become ready (published streams: $published, HTTP: ${http:-none})"
        fi
        sleep 1
    done
}

ensure_camera() {
    camera_spec "$1"
    write_camera_files
    local hash since rtsp_state hik_state
    hash="$(camera_hash)"
    since="$(date +%s)"
    if [[ "$(container_label "$RTSP_CONTAINER" "$LABEL.config")" != "$hash" ||
        "$(container_label "$HIK_CONTAINER" "$LABEL.config")" != "$hash" ]]; then
        log "creating $CAM ($CAM_IP, $CAM_NOTE)"
        create_camera "$hash"
    else
        rtsp_state="$(container_state "$RTSP_CONTAINER")"
        hik_state="$(container_state "$HIK_CONTAINER")"
        if [[ "$rtsp_state" != "running" ]]; then
            # The web container lives in the RTSP container's namespace: restart it after.
            docker stop -t 2 "$HIK_CONTAINER" >/dev/null 2>&1 || true
            docker start "$RTSP_CONTAINER" >/dev/null
            hik_state="exited"
        fi
        if [[ "$(cat "$STATE_DIR/mediamtx.yml" 2>/dev/null)" != \
            "$(cat "$STATE_DIR/mediamtx-open.yml")" ]]; then
            activate_config open
        elif [[ "$rtsp_state" == "running" && "$hik_state" == "running" ]]; then
            log "$CAM already running ($CAM_IP, $CAM_NOTE)"
            return 0
        fi
        [[ "$hik_state" == "running" ]] || docker start "$HIK_CONTAINER" >/dev/null
    fi
    wait_camera_ready "$since"
    log "$CAM ready: $CAM_IP, RTSP $CAM_RTSP_PORT, $CAM_NOTE"
}

# ---- commands -----------------------------------------------------------------------------------

cmd_up() {
    local count="${1:-$DEFAULT_CAMERAS}" k
    [[ "$count" =~ ^[1-5]$ ]] || die "up takes a camera count 1..$MAX_CAMERAS"
    require_docker
    require_images
    ensure_network
    prepare_media
    for k in $(numbers "$((count + 1))" "$MAX_CAMERAS"); do
        camera_spec "$k"
        if [[ "$(container_state "$RTSP_CONTAINER")" != "absent" ||
            "$(container_state "$HIK_CONTAINER")" != "absent" ]]; then
            log "removing $CAM (not requested)"
            docker rm -f "$HIK_CONTAINER" "$RTSP_CONTAINER" >/dev/null 2>&1 || true
        fi
    done
    for k in $(numbers 1 "$count"); do
        ensure_camera "$k"
    done
    cmd_status
}

cmd_down() {
    require_docker
    local ids
    ids="$(docker ps -aq --filter "label=$LABEL=1")"
    if [[ -n "$ids" ]]; then
        log "removing simulator containers"
        # shellcheck disable=SC2086
        docker rm -f $ids >/dev/null
    fi
    local network
    for network in "$NETWORK" "$UPLINK_NETWORK"; do
        if docker network inspect "$network" >/dev/null 2>&1; then
            log "removing network $network"
            docker network rm "$network" >/dev/null
        fi
    done
    log "simulator is down"
}

rtsp_mode() {
    if cmp -s "$STATE_DIR/mediamtx.yml" "$STATE_DIR/mediamtx-closed.yml"; then
        printf 'closed'
    else
        printf 'open'
    fi
}

cmd_status() {
    require_docker
    if docker network inspect "$NETWORK" >/dev/null 2>&1; then
        printf 'network %s %s (gateway %s), client address %s (sim.sh run)\n' "$NETWORK" \
            "$SUBNET" "$GATEWAY" "$CLIENT_IP"
    else
        printf 'network %s: absent (sim.sh up)\n' "$NETWORK"
    fi
    printf '%-5s %-14s %-8s %-8s %-11s %-5s %-6s %-9s %-9s %s\n' CAM IP RTSP-CTR HIK-CTR \
        RTSP CODEC ONVIF AUTH_FAIL LOCKED NOTE
    local k rtsp_state hik_state fails locks codec any=0
    for k in $(numbers 1 "$MAX_CAMERAS"); do
        camera_spec "$k"
        rtsp_state="$(container_state "$RTSP_CONTAINER")"
        hik_state="$(container_state "$HIK_CONTAINER")"
        [[ "$rtsp_state" == "absent" && "$hik_state" == "absent" ]] && continue
        any=1
        fails="-"
        locks="-"
        if [[ "$hik_state" != "absent" ]]; then
            fails="$(docker logs "$HIK_CONTAINER" 2>&1 | grep -c "AUTH_FAIL" || true)"
            locks="$(docker logs "$HIK_CONTAINER" 2>&1 | grep -c "] LOCKED ip=" || true)"
        fi
        codec="H.264"
        if [[ "$CAM_CODEC" == "h265" ]]; then
            codec="H.265"
        fi
        printf '%-5s %-14s %-8s %-8s %-11s %-5s %-6s %-9s %-9s %s\n' "$CAM" "$CAM_IP" \
            "$rtsp_state" "$hik_state" "$CAM_RTSP_PORT/$(rtsp_mode)" "$codec" "$CAM_ONVIF" \
            "$fails" "$locks" "$CAM_NOTE"
    done
    [[ "$any" == 1 ]] || printf '(no simulated cameras)\n'
}

require_camera() {
    camera_spec "$1"
    [[ "$(container_state "$RTSP_CONTAINER")" != "absent" ]] ||
        die "$CAM does not exist (sim.sh up)"
}

cmd_stop_camera() {
    require_docker
    require_camera "${1:-}"
    log "stopping $CAM (simulated power loss)"
    docker stop -t 2 "$HIK_CONTAINER" "$RTSP_CONTAINER" >/dev/null
}

cmd_start_camera() {
    require_docker
    require_camera "${1:-}"
    local since
    since="$(date +%s)"
    log "starting $CAM"
    if [[ "$(container_state "$RTSP_CONTAINER")" != "running" ]]; then
        docker stop -t 2 "$HIK_CONTAINER" >/dev/null 2>&1 || true
        docker start "$RTSP_CONTAINER" >/dev/null
    fi
    [[ "$(container_state "$HIK_CONTAINER")" == "running" ]] ||
        docker start "$HIK_CONTAINER" >/dev/null
    wait_camera_ready "$since"
    log "$CAM is back"
}

wait_rtsp_port() {
    # $1: open|closed, as seen from inside the camera on its LAN address.
    local want="$1" deadline state
    deadline=$(($(date +%s) + 30))
    while :; do
        if docker exec "$HIK_CONTAINER" bash -c \
            "exec 3<>/dev/tcp/$CAM_IP/$CAM_RTSP_PORT" >/dev/null 2>&1; then
            state="open"
        else
            state="closed"
        fi
        [[ "$state" == "$want" ]] && return 0
        [[ "$(date +%s)" -lt "$deadline" ]] || die "$CAM RTSP port still $state"
        sleep 0.5
    done
}

cmd_close_rtsp() {
    require_docker
    require_camera "${1:-}"
    [[ "$(container_state "$RTSP_CONTAINER")" == "running" ]] || die "$CAM is not running"
    activate_config closed
    wait_rtsp_port closed
    log "$CAM: RTSP port $CAM_RTSP_PORT closed (HTTP, SADP and SDK port still answer)"
}

cmd_open_rtsp() {
    require_docker
    require_camera "${1:-}"
    [[ "$(container_state "$RTSP_CONTAINER")" == "running" ]] || die "$CAM is not running"
    local since
    since="$(date +%s)"
    activate_config open
    wait_rtsp_port open
    wait_camera_ready "$since"
    log "$CAM: RTSP port $CAM_RTSP_PORT open again"
}

cmd_trigger_anpr() {
    require_docker
    require_camera "${1:-}"
    docker kill -s USR1 "$HIK_CONTAINER" >/dev/null
    log "$CAM: ANPR event triggered"
}

cmd_logs() {
    require_docker
    require_camera "${1:-}"
    case "${2:-both}" in
        rtsp) docker logs "$RTSP_CONTAINER" 2>&1 ;;
        hik) docker logs "$HIK_CONTAINER" 2>&1 ;;
        both)
            printf '== %s (MediaMTX)\n' "$RTSP_CONTAINER"
            docker logs --tail 40 "$RTSP_CONTAINER" 2>&1
            printf '== %s (fake_hikvision.py)\n' "$HIK_CONTAINER"
            docker logs --tail 40 "$HIK_CONTAINER" 2>&1
            ;;
        *) die "logs K [rtsp|hik]" ;;
    esac
}

client_container_args() {
    # Common `docker run` arguments of a client container on the camera LAN at address $1;
    # $2 = "uplink" also attaches the uplink network (then the camera LAN may not be eth0).
    mkdir -p "$BUILD_DIR" "$WORK_DIR"
    if [[ "${2:-}" == "uplink" ]]; then
        printf '%s\n' --network "name=$NETWORK,ip=$1" \
            --network "name=$UPLINK_NETWORK,gw-priority=1"
    else
        printf '%s\n' --network "$NETWORK" --ip "$1"
    fi
    printf '%s\n' --label "$LABEL=1" -v "$REPO_DIR:/src:ro" -v "$BUILD_DIR:/b" \
        -v "$WORK_DIR:/work" -v "$SIM_DIR:/sim:ro" -w /work --env-file "$ENV_FILE"
}

cmd_run() {
    require_docker
    docker network inspect "$NETWORK" >/dev/null 2>&1 || die "network $NETWORK absent (sim.sh up)"
    local args=() line tty=() uplink=""
    if [[ "${SIM_UPLINK:-0}" == "1" ]]; then
        ensure_uplink_network
        uplink="uplink"
    fi
    while IFS= read -r line; do
        args+=("$line")
    done < <(client_container_args "$CLIENT_IP" "$uplink")
    if [[ -t 0 && -t 1 ]]; then
        tty=(-it)
    fi
    [[ $# -gt 0 ]] || set -- bash
    # The working directory /work is writable (var/ state such as the camera registry and
    # status.json) and links config/, models/ and video/ to the read-only checkout.
    # SIM_CAMERA_INTERFACE names the camera LAN interface inside the container.
    docker run --rm ${tty[@]+"${tty[@]}"} --name kzcam-client --hostname sim-jetson \
        "${args[@]}" "$DEV_IMAGE" bash -c '
for d in config models video tools; do ln -sfn "/src/$d" "/work/$d"; done
SIM_CAMERA_INTERFACE="$(ip -o -4 addr show | awk "/ 192\\.168\\.77\\./ { print \$2; exit }")"
export SIM_CAMERA_INTERFACE
echo "sim: camera LAN interface $SIM_CAMERA_INTERFACE (192.168.77.5)" >&2
exec "$@"' bash "$@"
}

cmd_build() {
    local target="${1:-kz_anpr}"
    cmd_run bash -c "cmake -S /src -B /b -DCMAKE_BUILD_TYPE=Release \
        -DONNXRUNTIME_ROOT=/opt/onnxruntime -DKZ_ANPR_WITH_TENSORRT=OFF >/dev/null &&
        make -C /b -j4 $target"
}

cmd_clean() {
    cmd_down
    rm -rf "$CACHE_DIR"
    log "removed $CACHE_DIR"
}

# ---- verify -------------------------------------------------------------------------------------

VERIFY_PASS=0
VERIFY_FAIL=0

record() {
    # Runs a check, prints its output, counts PASS/FAIL lines.
    local output status=0
    output="$("$@" 2>&1)" || status=$?
    printf '%s\n' "$output"
    VERIFY_PASS=$((VERIFY_PASS + $(printf '%s\n' "$output" | grep -c '^PASS' || true)))
    VERIFY_FAIL=$((VERIFY_FAIL + $(printf '%s\n' "$output" | grep -c '^FAIL' || true)))
    if [[ "$status" -ne 0 && "$output" != "FAIL"* && "$output" != *$'\nFAIL'* ]]; then
        printf 'FAIL %s: exit status %d\n' "$*" "$status"
        VERIFY_FAIL=$((VERIFY_FAIL + 1))
    fi
}

check() {
    # A check inside the verification client ($1 = container), see verify_sim.sh.
    local container="$1"
    shift
    docker exec "$container" bash /sim/verify_sim.sh "$@"
}

host_check() {
    # PASS/FAIL line for a condition evaluated on the host: $1 name, rest = command.
    local name="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        printf 'PASS %s\n' "$name"
    else
        printf 'FAIL %s\n' "$name"
    fi
}

# Log checks read container logs since this verification started (logs survive restarts),
# whole: `grep -q` in a pipe would SIGPIPE `docker logs` and fail under pipefail.
log_count() {
    docker logs --since "$VERIFY_STARTED" "$1" 2>&1 | grep -c "$2" || true
}

log_count_is() {
    # log_count_is CONTAINER PATTERN N: waits briefly for N lines (fake_hikvision reads the
    # MediaMTX log every 0.5 s), then requires exactly N.
    local deadline=$(($(date +%s) + 4))
    while [[ "$(log_count "$1" "$2")" -lt "$3" && "$(date +%s)" -lt "$deadline" ]]; do
        sleep 0.5
    done
    sleep 1
    [[ "$(log_count "$1" "$2")" == "$3" ]]
}

start_client() {
    # A long-running verification client at address $2 named $1.
    local args=() line
    while IFS= read -r line; do
        args+=("$line")
    done < <(client_container_args "$2")
    docker rm -f "$1" >/dev/null 2>&1 || true
    docker run -d --name "$1" "${args[@]}" "$DEV_IMAGE" sleep infinity >/dev/null
}

cmd_verify() {
    require_docker
    load_env
    local running=() k v="kzcam-verify" lock="kzcam-lockout"
    for k in $(numbers 1 "$MAX_CAMERAS"); do
        camera_spec "$k"
        if [[ "$(container_state "$RTSP_CONTAINER")" == "running" &&
            "$(container_state "$HIK_CONTAINER")" == "running" ]]; then
            running+=("$k")
        fi
    done
    [[ "${#running[@]}" -ge 4 ]] || die "verify needs cameras 1..4 running (sim.sh up)"
    start_client "$v" "$VERIFY_IP"
    start_client "$lock" "$LOCKOUT_IP"
    trap 'docker rm -f kzcam-verify kzcam-lockout >/dev/null 2>&1 || true' EXIT
    VERIFY_STARTED="$(date +%s)"
    printf '== simulator verification from %s (and %s for the lockout)\n' "$VERIFY_IP" \
        "$LOCKOUT_IP"

    printf -- '-- RTSP: codec and size of every stream (ffprobe, TCP, digest)\n'
    for k in "${running[@]}"; do
        camera_spec "$k"
        local pass_env="HIKVISION_PASSWORD" codec="h264"
        if [[ "$k" == 4 ]]; then
            pass_env="SIM_CAM4_PASSWORD"
        fi
        if [[ "$CAM_CODEC" == "h265" ]]; then
            codec="hevc"
        fi
        if [[ "$CAM_ACTIVATED" == "true" ]]; then
            record check "$v" rtsp-probe "$CAM" "$CAM_IP" "$CAM_RTSP_PORT" \
                Streaming/Channels/101 "$codec" "$pass_env"
            record check "$v" rtsp-probe "$CAM" "$CAM_IP" "$CAM_RTSP_PORT" \
                Streaming/Channels/102 "$codec" "$pass_env"
        else
            record check "$v" rtsp-rejected "$CAM" "$CAM_IP" "$CAM_RTSP_PORT" HIKVISION_PASSWORD \
                CAMERA_NOT_ACTIVATED
        fi
    done
    printf -- '-- RTSP: rejected logins and a wrong stream path\n'
    camera_spec 1
    record check "$v" rtsp-rejected cam1-wrong-password "$CAM_IP" 554 SIM_WRONG_PASSWORD
    record host_check "cam1 counted that wrong RTSP password once (AUTH_FAIL source=rtsp)" \
        log_count_is "$HIK_CONTAINER" "AUTH_FAIL ip=$VERIFY_IP source=rtsp" 1
    camera_spec 4
    record check "$v" rtsp-rejected cam4-shared-credentials "$CAM_IP" 554 HIKVISION_PASSWORD
    camera_spec 1
    record check "$v" rtsp-bad-path cam1 "$CAM_IP" 554

    printf -- '-- RTSP: frames through GStreamer (rtspsrc, software decoders)\n'
    camera_spec 1
    record check "$v" gst-frames cam1 "$CAM_IP" 554 Streaming/Channels/101 h264
    camera_spec 2
    record check "$v" gst-frames cam2 "$CAM_IP" 554 Streaming/Channels/101 h265
    camera_spec 3
    record check "$v" gst-frames cam3 "$CAM_IP" 8554 Streaming/Channels/102 h264

    printf -- '-- Discovery\n'
    local ips=() k2
    for k2 in "${running[@]}"; do
        camera_spec "$k2"
        ips+=("$CAM_IP")
    done
    record check "$v" sadp "${ips[@]}"
    camera_spec 1
    record check "$v" sadp-field "$CAM_IP" Activated true
    if [[ " ${running[*]} " == *" 5 "* ]]; then
        camera_spec 5
        record check "$v" sadp-field "$CAM_IP" Activated false
        record check "$v" http-state cam5-not-activated "$CAM_IP" 401
        record check "$v" isapi-device-info cam5-not-activated "$CAM_IP" HIKVISION_PASSWORD 401
    fi
    camera_spec 2
    record check "$v" wsd-exactly "$CAM_IP"
    record check "$v" onvif-info cam2 "$CAM_IP"
    camera_spec 1
    record check "$v" onvif-disabled cam1 "$CAM_IP"
    record check "$v" tcp-state cam1-sdk "$CAM_IP" 8000 open

    printf -- '-- ISAPI (HTTP digest)\n'
    record check "$v" isapi-device-info cam1 "$CAM_IP" HIKVISION_PASSWORD 200
    record check "$v" isapi-device-info cam1-wrong-password "$CAM_IP" SIM_WRONG_PASSWORD 401
    record check "$v" isapi-channel cam1 "$CAM_IP" 101
    camera_spec 2
    record check "$v" isapi-channel cam2 "$CAM_IP" 101
    record check "$v" isapi-traffic cam2 "$CAM_IP"

    printf -- '-- Illegal-login lock (cam1, from %s)\n' "$LOCKOUT_IP"
    camera_spec 1
    record check "$lock" lockout cam1 "$CAM_IP"
    record host_check "cam1 logged LOCKED once for $LOCKOUT_IP" \
        log_count_is "$HIK_CONTAINER" "LOCKED ip=$LOCKOUT_IP after 5 failed logins" 1
    record check "$v" isapi-device-info cam1-other-client-unaffected "$CAM_IP" \
        HIKVISION_PASSWORD 200
    record host_check "cam1 never locked the verification client $VERIFY_IP" \
        log_count_is "$HIK_CONTAINER" "LOCKED ip=$VERIFY_IP" 0
    docker restart -t 2 "$HIK_CONTAINER" >/dev/null
    sleep 2
    record check "$lock" isapi-device-info cam1-unlocked-after-reboot "$CAM_IP" \
        HIKVISION_PASSWORD 200

    printf -- '-- alertStream (heartbeat + ANPR event, every 20 s)\n'
    record check "$v" alert-stream cam1 "$CAM_IP"

    printf -- '-- close-rtsp / open-rtsp (RTSP_PORT_CLOSED)\n'
    camera_spec 3
    cmd_close_rtsp 3 2>/dev/null
    record check "$v" tcp-state cam3-rtsp-closed "$CAM_IP" 8554 closed
    record check "$v" http-state cam3-web-still-up "$CAM_IP" 200
    record check "$v" sadp "$CAM_IP"
    cmd_open_rtsp 3 2>/dev/null
    record check "$v" rtsp-probe cam3-reopened "$CAM_IP" 8554 Streaming/Channels/101 h264 \
        HIKVISION_PASSWORD

    printf -- '-- stop-camera / start-camera (PoE loss, approximated)\n'
    camera_spec 1
    cmd_stop_camera 1 2>/dev/null
    record check "$v" tcp-state cam1-rtsp-gone "$CAM_IP" 554 unreachable
    record check "$v" tcp-state cam1-http-gone "$CAM_IP" 80 unreachable
    cmd_start_camera 1 2>/dev/null
    record check "$v" rtsp-probe cam1-back "$CAM_IP" 554 Streaming/Channels/101 h264 \
        HIKVISION_PASSWORD
    record check "$v" http-state cam1-back "$CAM_IP" 200

    printf '== verification: %d passed, %d failed\n' "$VERIFY_PASS" "$VERIFY_FAIL"
    [[ "$VERIFY_FAIL" -eq 0 ]]
}

usage() {
    # The header comment of this file.
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
}

main() {
    local command="${1:-help}"
    if [[ $# -gt 0 ]]; then
        shift
    fi
    case "$command" in
        up) load_env; cmd_up "$@" ;;
        down) cmd_down ;;
        status) load_env; cmd_status ;;
        stop-camera) load_env; cmd_stop_camera "$@" ;;
        start-camera) load_env; cmd_start_camera "$@" ;;
        close-rtsp) load_env; cmd_close_rtsp "$@" ;;
        open-rtsp) load_env; cmd_open_rtsp "$@" ;;
        trigger-anpr) load_env; cmd_trigger_anpr "$@" ;;
        logs) load_env; cmd_logs "$@" ;;
        run) cmd_run "$@" ;;
        build) cmd_build "$@" ;;
        verify) cmd_verify ;;
        clean) cmd_clean ;;
        help | -h | --help) usage ;;
        *) usage; die "unknown command '$command'" ;;
    esac
}

main "$@"
