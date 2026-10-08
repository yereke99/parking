#!/usr/bin/env bash
# Installs the live-camera service: systemd starts the ANPR at boot and restarts it whenever it
# stops (camera lost, power cut, crash).
#
#   tools/install_service.sh CAMERA [CAMERA_ID]    one camera by its URL or device
#   tools/install_service.sh --cameras             every Hikvision camera on the PoE switch
#   make service CAMERA='rtsp://user:password@192.168.1.64:554/Streaming/Channels/101'
#   make camera-service
#
# Both modes install the same kz-anpr unit, because only one ANPR process fits on the 4 GB Nano:
# installing one replaces the other.
#
# It runs `tools/jetson_docker.sh check` first, so the image, both models and their TensorRT
# engines are known to work before anything is installed; camera mode then runs the camera check
# with the credentials the service will use. Run it as the user that owns the checkout; it asks
# for sudo for the system files only.
#
# Camera mode takes HIKVISION_USERNAME / HIKVISION_PASSWORD from the environment, else from
# CAMERA_ENV_FILE (default config/cameras.env), else asks for them. They and every other
# credential variable found the same way (HIKVISION_* / ONVIF_*, and the names the camera config
# gives in username_env / password_env) are written to /etc/default/kz-anpr, readable by root
# only. CAMERA_CONFIG (default config/cameras.yaml) is the camera config inside the checkout.
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

usage() {
    echo "usage: $0 CAMERA [CAMERA_ID]" >&2
    echo "       $0 --cameras" >&2
}

mode=single
if [[ "${1:-}" == "--cameras" ]]; then
    mode=cameras
    shift
    if (( $# > 0 )); then
        usage
        exit 2
    fi
else
    camera="${1:-}"
    camera_id="${2:-gate-01}"
    if [[ -z "$camera" ]]; then
        usage
        exit 2
    fi
    if [[ "$camera$camera_id" == *"'"* || "$camera$camera_id" == *$'\n'* ]]; then
        echo "ERROR: the camera source and id must not contain single quotes or line breaks." >&2
        exit 2
    fi
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

staging="$(mktemp -d)"
stopped_service=0
installed=0
on_exit() {
    rm -rf "$staging"
    if (( stopped_service && ! installed )); then
        echo "kz-anpr was stopped for the checks and stays stopped;" \
            "sudo systemctl start kz-anpr brings the previous setup back." >&2
    fi
}
trap on_exit EXIT

# ---- Camera mode: configuration and credentials, gathered before anything is stopped ---------

credential_pattern='^(HIKVISION|ONVIF)_[A-Z0-9_]+$'
credential_names=()
credential_values=()
# Names from username_env / password_env in the camera config, space-separated and padded.
referenced_names=" "

is_credential_name() {
    [[ "$1" =~ $credential_pattern || "$referenced_names" == *" $1 "* ]]
}

# The same line-based read of username_env / password_env as tools/jetson_docker.sh.
referenced_env_names() {
    local line
    local pattern="^[[:space:]-]*(username_env|password_env)[[:space:]]*:[[:space:]]*[\"']?"
    pattern+="([A-Za-z_][A-Za-z0-9_]*)[\"']?[[:space:]]*(#.*)?\$"
    [[ -r "$1" ]] || return 0
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        if [[ "$line" =~ $pattern ]]; then
            echo "${BASH_REMATCH[2]}"
        fi
    done < "$1"
}

# Prints the index of credential $1, or -1.
credential_index() {
    local i
    for i in "${!credential_names[@]}"; do
        if [[ "${credential_names[$i]}" == "$1" ]]; then
            echo "$i"
            return
        fi
    done
    echo -1
}

set_credential() {
    local index
    index="$(credential_index "$1")"
    if (( index < 0 )); then
        credential_names+=("$1")
        credential_values+=("$2")
    else
        credential_values[index]="$2"
    fi
}

# True when credential $1 is known and not empty.
has_credential() {
    local index
    index="$(credential_index "$1")"
    (( index >= 0 )) && [[ -n "${credential_values[$index]}" ]]
}

# Reads the credential lines of an env file the way docker --env-file does, so the service gets
# exactly what `make camera-check` used: KEY=value, the value taken literally (quotes included),
# blank lines and # comments skipped.
read_env_file() {
    local file="$1" line name value number=0
    while IFS= read -r line || [[ -n "$line" ]]; do
        number=$((number + 1))
        line="${line%$'\r'}"
        line="${line#"${line%%[![:space:]]*}"}"
        if [[ -z "$line" || "$line" == \#* || "$line" != *=* ]]; then
            continue
        fi
        name="${line%%=*}"
        value="${line#*=}"
        if ! is_credential_name "$name"; then
            continue
        fi
        if [[ "$value" =~ ^\".*\"$ || "$value" =~ ^\'.*\'$ ]]; then
            echo "WARNING: $file line $number: $name is quoted; docker --env-file keeps the" \
                "quotes as part of the value. Write $name=value without quotes." >&2
        fi
        set_credential "$name" "$value"
    done < "$file"
}

# An EnvironmentFile value for systemd: unquoted, with a backslash before every character that
# is not plainly safe. Every systemd release reads an unquoted \X as X; quotes are not portable
# (inside double quotes 237, JetPack 4's version, drops the backslash of \X while 245 keeps it).
systemd_escape() {
    local value="$1" escaped="" char i
    for (( i = 0; i < ${#value}; i++ )); do
        char="${value:i:1}"
        case "$char" in
            [A-Za-z0-9_./:@%+,=-])
                escaped+="$char"
                ;;
            *)
                escaped+="\\$char"
                ;;
        esac
    done
    printf '%s' "$escaped"
}

prompt_credential() {
    local name="$1" label="$2" secret="$3" value="" again=""
    if [[ "$secret" == yes ]]; then
        if ! IFS= read -rs -p "$label ($name): " value; then
            value=""
        fi
        echo >&2
        if [[ -n "$value" ]]; then
            if ! IFS= read -rs -p "Repeat the $label: " again; then
                again=""
            fi
            echo >&2
            if [[ "$value" != "$again" ]]; then
                echo "ERROR: the two passwords differ." >&2
                exit 2
            fi
        fi
    elif ! IFS= read -r -p "$label ($name): " value; then
        value=""
    fi
    if [[ -z "$value" ]]; then
        echo "ERROR: no $name. Put HIKVISION_USERNAME and HIKVISION_PASSWORD in" \
            "config/cameras.env (see config/cameras.env.example) or enter them when asked." >&2
        exit 2
    fi
    set_credential "$name" "$value"
}

if [[ "$mode" == cameras ]]; then
    camera_config="${CAMERA_CONFIG:-config/cameras.yaml}"
    if [[ ! "$camera_config" =~ ^[A-Za-z0-9._/-]+$ || "$camera_config" == /* ||
          "/$camera_config/" == */../* ]]; then
        echo "ERROR: CAMERA_CONFIG must be a path inside the checkout, relative to it:" \
            "$camera_config" >&2
        exit 2
    fi
    if [[ ! -f "$camera_config" ]]; then
        echo "ERROR: $camera_config is missing." >&2
        exit 2
    fi

    referenced_names+="$(referenced_env_names "$camera_config" | tr '\n' ' ')"
    env_file="${CAMERA_ENV_FILE-config/cameras.env}"
    if [[ -n "$env_file" && -f "$env_file" ]]; then
        read_env_file "$env_file"
    fi
    # Exported variables win over the file, as with docker -e; an empty one does not.
    while IFS= read -r name; do
        if is_credential_name "$name" && [[ -n "${!name}" ]]; then
            set_credential "$name" "${!name}"
        fi
    done < <(compgen -e)

    if ! has_credential HIKVISION_USERNAME || ! has_credential HIKVISION_PASSWORD; then
        echo "The camera login (the same for every camera; per-camera logins go in" \
            "config/cameras.env) is stored in /etc/default/kz-anpr, readable by root only." >&2
    fi
    if ! has_credential HIKVISION_USERNAME; then
        prompt_credential HIKVISION_USERNAME "Camera username" no
    fi
    if ! has_credential HIKVISION_PASSWORD; then
        prompt_credential HIKVISION_PASSWORD "Camera password" yes
    fi
    for i in "${!credential_names[@]}"; do
        value="${credential_values[$i]}"
        if [[ "$value" == *$'\n'* || "$value" == *$'\r'* ]]; then
            echo "ERROR: ${credential_names[$i]} contains a line break." >&2
            exit 2
        fi
    done
    value=""

    # Written now, while the umask keeps it private; installed only after the checks pass.
    (
        umask 077
        {
            echo "# Camera mode of the kz-anpr service, written by"
            echo "# tools/install_service.sh --cameras. Readable by root only: it holds camera"
            echo "# passwords. Re-run make camera-service to change them."
            printf 'KZ_ANPR_CAMERA_CONFIG=%s\n' "$(systemd_escape "/workspace/$camera_config")"
            for i in "${!credential_names[@]}"; do
                printf '%s=%s\n' "${credential_names[$i]}" \
                    "$(systemd_escape "${credential_values[$i]}")"
            done
        } > "$staging/kz-anpr.env"
    )
fi

# ---- Checks ------------------------------------------------------------------------------------

# A running service holds the GPU; the check below needs it to itself on a 4 GB Nano.
if systemctl is-active --quiet kz-anpr 2>/dev/null; then
    echo "== Stopping the running kz-anpr service"
    sudo systemctl stop kz-anpr
    stopped_service=1
fi

echo "== Checking the image, the models and the TensorRT engines"
./tools/jetson_docker.sh check

if [[ "$mode" == cameras ]]; then
    echo "== Checking the cameras with the service's credentials (one login per camera)"
    set +e
    (
        for i in "${!credential_names[@]}"; do
            export "${credential_names[$i]}=${credential_values[$i]}"
        done
        export CAMERA_ENV_FILE=
        export KZ_ANPR_CAMERA_CONFIG="/workspace/$camera_config"
        exec ./tools/jetson_docker.sh cameras --camera-check
    ) 2>&1 | tee "$staging/camera-check.log"
    check_status="${PIPESTATUS[0]}"
    set -e
    case "$check_status" in
        0)
            ;;
        3)
            # Each failed login counts toward Hikvision's lockout (about 5 failures lock the
            # address for 30 minutes); a service retrying a wrong password would trip it.
            if grep -Eq 'RTSP_AUTH_FAILED|RTSP_CREDENTIALS_MISSING' \
                    "$staging/camera-check.log"; then
                echo "ERROR: a camera rejected the credentials, or has none." \
                    "Nothing was installed." >&2
                echo "Fix HIKVISION_USERNAME / HIKVISION_PASSWORD (or the per-camera variables)," \
                    "then run make camera-check and make camera-service again." >&2
                exit 3
            fi
            echo "WARNING: not every camera is READY (see above). The service starts anyway," \
                "processes the healthy cameras and keeps retrying the others." >&2
            ;;
        *)
            echo "ERROR: the camera check failed with exit code $check_status (see above)." \
                "Nothing was installed." >&2
            exit "$check_status"
            ;;
    esac
fi

# ---- Install -----------------------------------------------------------------------------------

echo "== Installing the kz-anpr service"
if [[ "$mode" == cameras ]]; then
    unit_template=deploy/systemd/kz-anpr-cameras.service
else
    unit_template=deploy/systemd/kz-anpr.service
    printf "CAMERA='%s'\nCAMERA_ID='%s'\n" "$camera" "$camera_id" > "$staging/kz-anpr.env"
fi
sed -e "s|@PROJECT_DIR@|$project_dir|g" -e "s|@USER@|$user|g" \
    "$unit_template" > "$staging/kz-anpr.service"
sed -e "s|@PROJECT_DIR@|$project_dir|g" deploy/logrotate/kz-anpr > "$staging/kz-anpr.logrotate"

sudo install -m 0644 -o root -g root "$staging/kz-anpr.service" /etc/systemd/system/kz-anpr.service
sudo install -m 0600 -o root -g root "$staging/kz-anpr.env" /etc/default/kz-anpr
sudo install -m 0644 -o root -g root "$staging/kz-anpr.logrotate" /etc/logrotate.d/kz-anpr
sudo systemctl daemon-reload
sudo systemctl enable kz-anpr
sudo systemctl restart kz-anpr
installed=1

echo
if [[ "$mode" == cameras ]]; then
    echo "kz-anpr is running on every camera of the PoE switch and starts at boot."
    echo "  cameras: make camera-status"
    echo "  credentials: ${credential_names[*]} in /etc/default/kz-anpr (root only)"
else
    echo "kz-anpr is running and starts at boot."
fi
echo "  logs:    journalctl -u kz-anpr -f"
echo "  plates:  tail -F $project_dir/var/events.jsonl"
echo "  stop:    sudo systemctl stop kz-anpr    (before make run / make check)"
echo "  remove:  sudo systemctl disable --now kz-anpr"
