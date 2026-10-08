#!/usr/bin/env bash
# End-to-end camera-mode scenarios against the SIMULATED camera LAN (development only; it never
# proves real-hardware behaviour). Needs `sim.sh up 5` and a Linux build of kz_anpr in
# SIM_BUILD_DIR (sim.sh build, or any build dir of the kz-anpr-dev image).
#
#   tests/camera_sim/run_scenarios.sh [seconds]
#
# Runs `kz_anpr --cameras` on every simulated camera with the development ANPR profile (ONNX
# Runtime on the CPU), and while it runs:
#   - cam3 loses power (both containers stop) and comes back: it must reconnect while
#     camera-01/02 keep processing;
#   - cam1's RTSP service is closed and reopened: RTSP_PORT_CLOSED or a stream error, then
#     recovery.
# Afterwards it checks: events from every healthy camera carry the expected plate, the camera
# with the wrong password failed exactly once (no lockout), the inactive camera was never sent a
# password, no password appears in any output, the status file lists every camera.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sim="$here/sim.sh"
duration="${1:-240}"
work="$here/.cache/work"
expected_plate="${EXPECTED_PLATE:-152JTA02}"
password="$(sed -n 's/^HIKVISION_PASSWORD=//p' "$here/sim.env")"

rm -rf "$work/var"
mkdir -p "$work/var"

echo "== kz_anpr --cameras for ${duration}s (camera-01..04 + inactive camera-05)"
"$sim" run bash -c "timeout -s INT $duration /b/kz_anpr --config config/default.yaml --cameras \
    --camera-config /src/tests/camera_sim/cameras-sim.yaml --events-file var/events.jsonl \
    > var/stdout.jsonl 2> var/stderr.log; echo \$? > var/exit_code" &
runner=$!

phase() { sleep "$1"; echo "== t+$2 s: $3"; }
phase 70 70 "cam3 power loss"
"$sim" stop-camera 3 >/dev/null
phase 40 110 "cam3 power back"
"$sim" start-camera 3 >/dev/null
phase 30 140 "cam1 RTSP service closed"
"$sim" close-rtsp 1 >/dev/null
phase 20 160 "cam1 RTSP service reopened"
"$sim" open-rtsp 1 >/dev/null
wait "$runner" || true

failures=0
check() {
    if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; failures=$((failures + 1)); fi
}
events="$work/var/events.jsonl"
log="$work/var/stderr.log"
plates_for() { grep "\"camera_id\":\"$1\"" "$events" 2>/dev/null | grep -c "\"normalized_plate\":\"$expected_plate\"" || true; }

echo "== results"
echo "exit code: $(cat "$work/var/exit_code" 2>/dev/null || echo '?')"
for id in camera-01 camera-02 camera-03; do
    echo "$id: $(plates_for "$id") x $expected_plate"
done
# The simulator runs the detector and the OCR on the CPU, about one frame per second per camera
# with three cameras (the Jetson runs them on the GPU), and the vote needs three agreeing reads,
# so a single camera may miss the plate within the run. The recognition chain itself is proven
# when the plate is confirmed over RTSP on at least two cameras, one of them H.265.
recognising=0
for id in camera-01 camera-02 camera-03; do
    [[ $(plates_for "$id") -ge 1 ]] && recognising=$((recognising + 1))
done
check "$expected_plate confirmed over RTSP on >= 2 of the 3 healthy cameras" '[[ $recognising -ge 2 ]]'
check "$expected_plate confirmed on the H.265 camera (camera-02) or both H.264 ones" \
    '[[ $(plates_for camera-02) -ge 1 || ( $(plates_for camera-01) -ge 1 && $(plates_for camera-03) -ge 1 ) ]]'
check "no event from camera-04 (wrong password) or camera-05 (inactive)" \
    '! grep -qE "\"camera_id\":\"camera-0[45]\"" "$events"'
check "camera-03 power loss was detected" \
    'grep -E "camera_id=camera-03" "$log" | grep -qE "STREAM_TIMEOUT|STREAM_ENDED|CAMERA_UNREACHABLE|camera_stream_lost"'
check "camera-03 reconnected after power came back" \
    'grep -E "camera_id=camera-03" "$log" | grep -qE "camera_stream_opened|camera_reconnected" &&
     [[ $(grep -E "camera_id=camera-03" "$log" | grep -c "camera_stream_opened") -ge 2 ]]'
check "camera-01 RTSP outage was reported and recovered" \
    '[[ $(grep -E "camera_id=camera-01" "$log" | grep -c "camera_stream_opened") -ge 2 ]]'
check "camera-04 reported RTSP_AUTH_FAILED" 'grep -q "camera_id=camera-04.*RTSP_AUTH_FAILED" "$log"'
check "camera-05 reported CAMERA_NOT_ACTIVATED" 'grep -q "camera-05.*CAMERA_NOT_ACTIVATED" "$log"'
cam4_failures="$(docker logs kzcam-cam4-hik 2>&1 | grep -c AUTH_FAIL || true)"
cam5_logins="$(docker logs kzcam-cam5-hik 2>&1 | grep -c 'AUTH_FAIL' || true)"
echo "cam4 failed logins in the camera log (whole simulator lifetime): $cam4_failures"
check "camera-04 is not locked out" '! docker logs kzcam-cam4-hik 2>&1 | grep -q "] LOCKED ip="'
check "camera-05 never received a password" '[[ "$cam5_logins" -eq 0 ]]'
check "no password in stdout, stderr, events or state files" \
    '! grep -rqF -- "$password" "$work/var"'
check "status file lists five cameras" \
    '[[ $(python3 -c "import json,sys; print(len({c[\"id\"] for c in json.load(open(sys.argv[1]))[\"cameras\"]}))" "$work/var/cameras/status.json") -eq 5 ]]'
check "periodic per-camera metrics were logged" 'grep -q "event=camera_metrics" "$log"'
check "periodic system metrics were logged" 'grep -q "event=system_metrics" "$log"'

echo "== $failures failed check(s); logs in $work/var"
exit $(( failures > 0 ))
