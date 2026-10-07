#!/bin/sh
# Usage: fetch-resumable URL SHA256 OUTPUT [LOCAL_COPY]
#
# Like fetch-verified, but a dropped or stalled transfer resumes where it stopped instead of
# starting over: nomeroff.net.ua resets long downloads to the Nano. When LOCAL_COPY exists and has
# the expected SHA-256 (a file placed in the build context by hand), it is used without any
# download. A server without byte ranges falls back to a fresh transfer on the next attempt.
set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
    echo "usage: fetch-resumable URL SHA256 OUTPUT [LOCAL_COPY]" >&2
    exit 2
fi

url=$1
expected_sha256=$2
output=$3
local_copy=${4:-}
attempts=40

verified() {
    echo "$expected_sha256  $1" | sha256sum -c - >/dev/null 2>&1
}

if [ -n "$local_copy" ] && [ -f "$local_copy" ]; then
    if verified "$local_copy"; then
        cp "$local_copy" "$output"
        echo "fetch-resumable: using $local_copy from the build context"
        exit 0
    fi
    echo "fetch-resumable: $local_copy does not match the expected SHA-256; downloading" >&2
fi

rm -f "$output"
attempt=1
while :; do
    if curl --http1.1 --location --fail --connect-timeout 30 --speed-limit 1024 --speed-time 60 \
            --continue-at - --output "$output" "$url"; then
        break
    else
        status=$?
    fi
    # A transfer that completed but reported an error (for example a resume at end of file).
    if verified "$output"; then
        break
    fi
    # 33: the server refused a byte range. Start the next attempt from zero.
    if [ "$status" -eq 33 ]; then
        rm -f "$output"
    fi
    if [ "$attempt" -ge "$attempts" ]; then
        echo "fetch-resumable: $url failed after $attempts attempts" >&2
        exit 1
    fi
    received=$(wc -c < "$output" 2>/dev/null || echo 0)
    echo "fetch-resumable: attempt $attempt/$attempts stopped (curl $status) at $received bytes;" \
         "resuming in 10 s" >&2
    attempt=$((attempt + 1))
    sleep 10
done

echo "$expected_sha256  $output" | sha256sum -c -
