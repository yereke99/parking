#!/bin/sh
# Usage: fetch-verified URL SHA256 OUTPUT
#
# Downloads URL to OUTPUT and verifies its SHA-256. GitHub release downloads on the Nano have been
# unreliable, so each attempt restarts the transfer over HTTP/1.1, up to ten times. A checksum
# mismatch is never retried.
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: fetch-verified URL SHA256 OUTPUT" >&2
    exit 2
fi

url=$1
expected_sha256=$2
output=$3
attempts=10
attempt=1

until curl --http1.1 --location --fail --connect-timeout 30 --max-time 1800 \
        --output "$output" "$url"; do
    rm -f "$output"
    if [ "$attempt" -ge "$attempts" ]; then
        echo "fetch-verified: $url failed after $attempts attempts" >&2
        exit 1
    fi
    echo "fetch-verified: attempt $attempt/$attempts failed for $url; retrying in 10 s" >&2
    attempt=$((attempt + 1))
    sleep 10
done

echo "$expected_sha256  $output" | sha256sum -c -
