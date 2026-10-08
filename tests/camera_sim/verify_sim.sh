#!/usr/bin/env bash
# Checks of the SIMULATED camera LAN, run by `sim.sh verify` inside a client container on the
# kzcam network (kz-anpr-dev image: ffprobe, gst-launch-1.0, curl, python3). Each check prints
# "PASS <name>: <detail>" or "FAIL <name>: <detail>" and returns non-zero on failure.
# Credentials come from the environment (sim.env); URLs are redacted in everything printed.
#
#   verify_sim.sh rtsp-probe LABEL IP PORT PATH CODEC PASSWORD_VAR
#   verify_sim.sh rtsp-rejected LABEL IP PORT PASSWORD_VAR [SCENARIO]
#   verify_sim.sh rtsp-bad-path LABEL IP PORT
#   verify_sim.sh gst-frames LABEL IP PORT PATH h264|h265
#   verify_sim.sh sadp IP...            every IP answers the SADP inquiry
#   verify_sim.sh sadp-field IP FIELD VALUE
#   verify_sim.sh wsd-exactly IP...     exactly these IPs answer WS-Discovery
#   verify_sim.sh onvif-info LABEL IP
#   verify_sim.sh onvif-disabled LABEL IP
#   verify_sim.sh isapi-device-info LABEL IP PASSWORD_VAR EXPECTED_HTTP_STATUS
#   verify_sim.sh isapi-channel LABEL IP CHANNEL
#   verify_sim.sh isapi-traffic LABEL IP
#   verify_sim.sh lockout LABEL IP
#   verify_sim.sh alert-stream LABEL IP
#   verify_sim.sh tcp-state LABEL IP PORT open|closed|unreachable
#   verify_sim.sh http-state LABEL IP EXPECTED_HTTP_STATUS
set -uo pipefail

# GStreamer check: at least this many decoded frames within GST_SECONDS.
FRAMES=60
GST_SECONDS=12
USER_NAME="${HIKVISION_USERNAME:?HIKVISION_USERNAME not set (sim.env)}"
WRONG_PASSWORD="wrong-Pass_000"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

pass() { printf 'PASS %s: %s\n' "$1" "$2"; }
fail() {
    printf 'FAIL %s: %s\n' "$1" "$2"
    return 1
}
redact() { sed -E 's#(rtsp|http)://[^/@ ]*@#\1://<redacted>@#g'; }

secret() {
    # The password held by environment variable $1 (SIM_WRONG_PASSWORD: a wrong one).
    if [[ "$1" == "SIM_WRONG_PASSWORD" ]]; then
        printf '%s' "$WRONG_PASSWORD"
    else
        printf '%s' "${!1:?$1 not set (sim.env)}"
    fi
}

url_quote() {
    python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$1"
}

rtsp_url() {
    # rtsp_url IP PORT PATH PASSWORD: rtsp://user:password@ip:port/path, userinfo encoded.
    printf 'rtsp://%s:%s@%s:%s/%s' "$(url_quote "$USER_NAME")" "$(url_quote "$4")" "$1" "$2" "$3"
}

ffprobe_rtsp() {
    timeout 25 ffprobe -v error -rtsp_transport tcp \
        -show_entries stream=codec_name,profile,width,height,r_frame_rate -of compact=p=0 "$1" 2>&1
}

check_rtsp_probe() {
    local label="$1" ip="$2" port="$3" path="$4" want="$5" password output codec
    password="$(secret "$6")"
    output="$(ffprobe_rtsp "$(rtsp_url "$ip" "$port" "$path" "$password")" | redact)"
    codec="$(printf '%s\n' "$output" | grep -o 'codec_name=[a-z0-9]*' | head -1 | cut -d= -f2)"
    if [[ "$codec" == "$want" ]]; then
        pass "rtsp $label $path" "$(printf '%s\n' "$output" | grep -m1 'codec_name=')"
    else
        fail "rtsp $label $path" "expected $want, got: $(printf '%s' "$output" | tr '\n' ' ')"
    fi
}

check_rtsp_rejected() {
    local label="$1" ip="$2" port="$3" scenario="${5:-RTSP_AUTH_FAILED}" password output
    password="$(secret "$4")"
    output="$(ffprobe_rtsp "$(rtsp_url "$ip" "$port" Streaming/Channels/101 "$password")" |
        redact)"
    if [[ "$output" == *"401 Unauthorized"* ]]; then
        pass "rtsp $label" "401 Unauthorized ($scenario scenario)"
    else
        fail "rtsp $label" "expected 401, got: $(printf '%s' "$output" | tr '\n' ' ')"
    fi
}

check_rtsp_bad_path() {
    local label="$1" ip="$2" port="$3" output
    output="$(ffprobe_rtsp "$(rtsp_url "$ip" "$port" Streaming/Channels/999 \
        "$(secret HIKVISION_PASSWORD)")" | redact)"
    if [[ "$output" == *"404 Not Found"* ]]; then
        pass "rtsp $label Streaming/Channels/999" "404 Not Found (RTSP_STREAM_PATH_INVALID)"
    else
        fail "rtsp $label bad path" "expected 404, got: $(printf '%s' "$output" | tr '\n' ' ')"
    fi
}

check_gst_frames() {
    # Decoded frames reaching a verbose fakesink in GST_SECONDS (rtspsrc does not end on a
    # downstream EOS, so the pipeline is stopped by timeout and its buffers are counted).
    local label="$1" ip="$2" port="$3" path="$4" codec="$5" output frames
    output="$(timeout "$GST_SECONDS" gst-launch-1.0 -v rtspsrc \
        "location=rtsp://$ip:$port/$path" protocols=tcp latency=200 "user-id=$USER_NAME" \
        "user-pw=$(secret HIKVISION_PASSWORD)" ! "rtp${codec}depay" ! "${codec}parse" \
        ! "avdec_${codec}" ! fakesink sync=false silent=false 2>&1)"
    frames="$(printf '%s\n' "$output" | grep -c "last-message = chain" || true)"
    if [[ "$frames" -ge "$FRAMES" ]]; then
        pass "gstreamer $label $path" \
            "$frames frames decoded by avdec_$codec in ${GST_SECONDS}s (rtspsrc protocols=tcp)"
    else
        fail "gstreamer $label $path" "$frames frames: $(printf '%s\n' "$output" | redact |
            grep -v "last-message" | tail -3 | tr '\n' ' ')"
    fi
}

check_sadp() {
    local output
    output="$(python3 /sim/sadp_probe.py sadp --timeout 3 --expect "$@" 2>&1)"
    if [[ $? -eq 0 ]]; then
        pass "sadp" "ProbeMatch from $*"
    else
        fail "sadp" "missing answers"
    fi
    printf '%s\n' "$output" | sed 's/^/     /'
}

check_sadp_field() {
    local ip="$1" field="$2" want="$3" value
    value="$(python3 /sim/sadp_probe.py sadp --timeout 3 --json 2>&1 | python3 -c '
import json, sys
ip, field = sys.argv[1], sys.argv[2]
print(next((d.get(field, "?") for d in json.load(sys.stdin) if d.get("IPv4Address") == ip),
           "no answer"))' "$ip" "$field" 2>&1)"
    if [[ "$value" == "$want" ]]; then
        pass "sadp $ip" "$field=$value"
    else
        fail "sadp $ip" "$field expected $want, got $value"
    fi
}

check_wsd_exactly() {
    local output answered
    output="$(python3 /sim/sadp_probe.py wsd --timeout 3 --json 2>&1)"
    answered="$(printf '%s' "$output" | python3 -c '
import json, sys
print(" ".join(sorted({m["from"] for m in json.load(sys.stdin)})))' 2>&1)"
    if [[ "$answered" == "$(printf '%s\n' "$@" | sort | tr '\n' ' ' | sed 's/ $//')" ]]; then
        pass "ws-discovery" "only $answered answered (ONVIF off elsewhere)"
    else
        fail "ws-discovery" "expected exactly $*, answered: $answered"
    fi
}

check_onvif_info() {
    local label="$1" ip="$2" output
    output="$(python3 /sim/sadp_probe.py onvif-info "$ip" 2>&1)"
    if [[ $? -eq 0 && "$output" == *"GetDeviceInformation OK"* ]]; then
        pass "onvif $label" "$(printf '%s' "$output" | tail -1)"
    else
        fail "onvif $label" "$(printf '%s' "$output" | tr '\n' ' ')"
    fi
}

check_onvif_disabled() {
    local label="$1" ip="$2" status
    status="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X POST \
        -H 'Content-Type: application/soap+xml' --data '<x/>' "http://$ip/onvif/device_service")"
    if [[ "$status" == "404" ]]; then
        pass "onvif $label" "/onvif/device_service 404 (ONVIF disabled, as shipped)"
    else
        fail "onvif $label" "expected 404, got $status"
    fi
}

isapi_get() {
    # isapi_get IP PATH PASSWORD_VAR -> HTTP status; body in $TMP_DIR/body
    curl -s --digest -u "$USER_NAME:$(secret "$3")" -o "$TMP_DIR/body" -w '%{http_code}' \
        --max-time 5 "http://$1$2"
}

xml_fields() {
    # Values of the named elements (any namespace) of $TMP_DIR/body, "name=value" each.
    python3 - "$TMP_DIR/body" "$@" <<'PY'
import sys
import xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
values = {}
for item in root.iter():
    values.setdefault(item.tag.rsplit("}", 1)[-1], (item.text or "").strip())
print(" ".join("%s=%s" % (name, values.get(name, "?")) for name in sys.argv[2:]))
PY
}

check_isapi_device_info() {
    local label="$1" ip="$2" status
    status="$(isapi_get "$ip" /ISAPI/System/deviceInfo "$3")"
    if [[ "$status" != "$4" ]]; then
        fail "isapi $label deviceInfo" "expected HTTP $4, got $status"
    elif [[ "$status" == "200" ]]; then
        pass "isapi $label deviceInfo" "200 $(xml_fields model serialNumber macAddress \
            firmwareVersion deviceID)"
    else
        pass "isapi $label deviceInfo" "$status $(xml_fields lockStatus retryLoginTime)"
    fi
}

check_isapi_channel() {
    local label="$1" ip="$2" channel="$3" status
    status="$(isapi_get "$ip" "/ISAPI/Streaming/channels/$channel" HIKVISION_PASSWORD)"
    if [[ "$status" == "200" ]]; then
        pass "isapi $label channel $channel" "$(xml_fields videoCodecType videoResolutionWidth \
            videoResolutionHeight maxFrameRate GovLength)"
    else
        fail "isapi $label channel $channel" "HTTP $status"
    fi
}

check_isapi_traffic() {
    local label="$1" ip="$2" status
    status="$(isapi_get "$ip" /ISAPI/Traffic/capabilities HIKVISION_PASSWORD)"
    if [[ "$status" == "200" ]] && grep -q "<isSupportANPR>true" "$TMP_DIR/body"; then
        pass "isapi $label Traffic/capabilities" "200 $(xml_fields isSupportANPR \
            isSupportVehicleDetection)"
    else
        fail "isapi $label Traffic/capabilities" "HTTP $status"
    fi
}

check_lockout() {
    local label="$1" ip="$2" attempt status statuses=""
    for attempt in 1 2 3 4 5; do
        statuses+="$(isapi_get "$ip" /ISAPI/System/deviceInfo SIM_WRONG_PASSWORD) "
    done
    if [[ "$statuses" == "401 401 401 401 401 " ]]; then
        pass "lockout $label wrong password x5" "HTTP $statuses"
    else
        fail "lockout $label wrong password x5" "HTTP $statuses"
    fi
    status="$(isapi_get "$ip" /ISAPI/System/deviceInfo HIKVISION_PASSWORD)"
    if [[ "$status" == "401" ]] && grep -q "<lockStatus>lock</lockStatus>" "$TMP_DIR/body"; then
        pass "lockout $label right password while locked" "401 $(xml_fields lockStatus \
            unlockTime)"
    else
        fail "lockout $label right password while locked" "HTTP $status"
    fi
}

ALERT_PY='
import re, sys, time
deadline = time.time() + 40
data = b""
heartbeats = 0
plate = None
pictures = []
expected_pictures = None
stream = sys.stdin.buffer
while time.time() < deadline:
    chunk = stream.read1(65536) if hasattr(stream, "read1") else stream.read(4096)
    if not chunk:
        break
    data += chunk
    while True:
        start = data.find(b"--boundary\r\n")
        end = data.find(b"\r\n\r\n", start) if start >= 0 else -1
        if end < 0:
            break
        headers = data[start + 12:end].decode("ascii", "replace")
        length = int(re.search(r"Content-Length: (\d+)", headers).group(1))
        if len(data) < end + 4 + length:
            break
        body = data[end + 4:end + 4 + length]
        data = data[end + 4 + length:]
        if "image/jpeg" in headers:
            pictures.append((re.search(r"filename=\"([^\"]+)\"", headers).group(1), len(body)))
        elif b"<eventType>videoloss</eventType>" in body:
            heartbeats += 1
        elif b"<eventType>ANPR</eventType>" in body:
            plate = re.search(rb"<licensePlate>([^<]*)</licensePlate>", body).group(1).decode()
            country = re.search(rb"<country>([^<]*)</country>", body).group(1).decode()
            expected_pictures = int(re.search(rb"<picNum>(\d+)</picNum>", body).group(1))
    if plate is not None and len(pictures) >= expected_pictures:
        print("heartbeats=%d licensePlate=%s country=%s pictures=%s" % (
            heartbeats, plate, country, ",".join("%s(%dB)" % p for p in pictures)))
        sys.exit(0 if heartbeats >= 1 and plate == "152JTA02" and country == "30" else 1)
print("heartbeats=%d licensePlate=%s (no complete ANPR event)" % (heartbeats, plate))
sys.exit(1)
'

check_alert_stream() {
    # The parser exits after the first complete ANPR event (curl then stops on SIGPIPE).
    local label="$1" ip="$2" output status
    output="$(curl -sS -N --digest -u "$USER_NAME:$(secret HIKVISION_PASSWORD)" --max-time 45 \
        -D "$TMP_DIR/headers" "http://$ip/ISAPI/Event/notification/alertStream" 2>/dev/null |
        python3 -c "$ALERT_PY"; printf '\nstatus=%s' "${PIPESTATUS[1]}")"
    status="${output##*status=}"
    output="$(printf '%s' "${output%status=*}" | tr -s '\n' ' ')"
    if [[ "$status" -eq 0 ]] &&
        grep -qi "^content-type: multipart/mixed; boundary=boundary" "$TMP_DIR/headers"; then
        pass "alertStream $label" "multipart/mixed chunked: $output"
    else
        fail "alertStream $label" "status $status: $output"
    fi
}

check_tcp_state() {
    local label="$1" ip="$2" port="$3" want="$4" state
    state="$(python3 - "$ip" "$port" <<'PY'
import errno, socket, sys
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(4)
try:
    sock.connect((sys.argv[1], int(sys.argv[2])))
    print("open")
except ConnectionRefusedError:
    print("closed")
except (socket.timeout, OSError) as error:
    print("unreachable" if isinstance(error, socket.timeout) or error.errno in (
        errno.EHOSTUNREACH, errno.ENETUNREACH, errno.ETIMEDOUT) else "error %s" % error)
PY
)"
    if [[ "$state" == "$want" ]]; then
        pass "tcp $label" "$ip:$port $state"
    else
        fail "tcp $label" "$ip:$port expected $want, got $state"
    fi
}

check_http_state() {
    local label="$1" ip="$2" want="$3" status
    status="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://$ip/")"
    if [[ "$status" == "$want" ]]; then
        pass "http $label" "GET / $status"
    else
        fail "http $label" "GET / expected $want, got $status"
    fi
}

command="${1:-}"
[[ $# -gt 0 ]] && shift
case "$command" in
    rtsp-probe) check_rtsp_probe "$@" ;;
    rtsp-rejected) check_rtsp_rejected "$@" ;;
    rtsp-bad-path) check_rtsp_bad_path "$@" ;;
    gst-frames) check_gst_frames "$@" ;;
    sadp) check_sadp "$@" ;;
    sadp-field) check_sadp_field "$@" ;;
    wsd-exactly) check_wsd_exactly "$@" ;;
    onvif-info) check_onvif_info "$@" ;;
    onvif-disabled) check_onvif_disabled "$@" ;;
    isapi-device-info) check_isapi_device_info "$@" ;;
    isapi-channel) check_isapi_channel "$@" ;;
    isapi-traffic) check_isapi_traffic "$@" ;;
    lockout) check_lockout "$@" ;;
    alert-stream) check_alert_stream "$@" ;;
    tcp-state) check_tcp_state "$@" ;;
    http-state) check_http_state "$@" ;;
    *)
        awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
        exit 2
        ;;
esac
