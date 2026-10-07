#!/usr/bin/env bash
# Installs the live-camera service: systemd starts the ANPR at boot and restarts it whenever it
# stops (camera lost, power cut, crash).
#
#   tools/install_service.sh CAMERA [CAMERA_ID]
#   make service CAMERA='rtsp://user:password@192.168.1.64:554/Streaming/Channels/101'
#
# It runs `tools/jetson_docker.sh check` first, so the image, both models and their TensorRT
# engines are known to work before anything is installed. Run it as the user that owns the
# checkout; it asks for sudo for the system files only.
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

camera="${1:-}"
camera_id="${2:-gate-01}"
if [[ -z "$camera" ]]; then
    echo "usage: $0 CAMERA [CAMERA_ID]" >&2
    exit 2
fi
if [[ "$camera$camera_id" == *"'"* || "$camera$camera_id" == *$'\n'* ]]; then
    echo "ERROR: the camera source and id must not contain single quotes or line breaks." >&2
    exit 2
fi
if [[ "$project_dir" =~ [[:space:]] ]]; then
    echo "ERROR: the checkout path must not contain spaces: $project_dir" >&2
    exit 2
fi
if [[ "$(id -u)" == 0 ]]; then
    echo "ERROR: run this as the user that owns the checkout, not as root." >&2
    exit 2
fi
user="$(id -un)"
if ! id -nG "$user" | tr ' ' '\n' | grep -qx docker; then
    echo "ERROR: $user must be in the docker group for the service to start containers:" >&2
    echo "  sudo usermod -aG docker $user    then log out and back in" >&2
    exit 3
fi

# A running service holds the GPU; the check below needs it to itself on a 4 GB Nano.
if systemctl is-active --quiet kz-anpr 2>/dev/null; then
    echo "== Stopping the running kz-anpr service"
    sudo systemctl stop kz-anpr
fi

echo "== Checking the image, the models and the TensorRT engines"
./tools/jetson_docker.sh check

echo "== Installing the kz-anpr service"
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
sed -e "s|@PROJECT_DIR@|$project_dir|g" -e "s|@USER@|$user|g" \
    deploy/systemd/kz-anpr.service > "$staging/kz-anpr.service"
sed -e "s|@PROJECT_DIR@|$project_dir|g" deploy/logrotate/kz-anpr > "$staging/kz-anpr.logrotate"
printf "CAMERA='%s'\nCAMERA_ID='%s'\n" "$camera" "$camera_id" > "$staging/kz-anpr.env"

sudo install -m 0644 -o root -g root "$staging/kz-anpr.service" /etc/systemd/system/kz-anpr.service
sudo install -m 0600 -o root -g root "$staging/kz-anpr.env" /etc/default/kz-anpr
sudo install -m 0644 -o root -g root "$staging/kz-anpr.logrotate" /etc/logrotate.d/kz-anpr
sudo systemctl daemon-reload
sudo systemctl enable kz-anpr
sudo systemctl restart kz-anpr

echo
echo "kz-anpr is running and starts at boot."
echo "  logs:    journalctl -u kz-anpr -f"
echo "  plates:  tail -F $project_dir/var/events.jsonl"
echo "  stop:    sudo systemctl stop kz-anpr    (before make run / make check)"
echo "  remove:  sudo systemctl disable --now kz-anpr"
