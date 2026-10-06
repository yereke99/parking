#!/bin/sh
# Usage: fetch-wheels MANIFEST DEST_DIR
#
# Downloads every "<sha256>  <url>" line of MANIFEST into DEST_DIR through fetch-verified
# (retries plus SHA-256 check), so pip can then install from DEST_DIR with --no-index.
# Blank lines and lines starting with # are ignored.
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: fetch-wheels MANIFEST DEST_DIR" >&2
    exit 2
fi

manifest=$1
dest=$2
mkdir -p "$dest"

while read -r sha256 url extra || [ -n "${sha256:-}" ]; do
    case "$sha256" in
        '' | '#'*) continue ;;
    esac
    if [ -z "$url" ] || [ -n "$extra" ]; then
        echo "fetch-wheels: malformed line in $manifest: $sha256 $url $extra" >&2
        exit 1
    fi
    fetch-verified "$url" "$sha256" "$dest/${url##*/}" </dev/null
done < "$manifest"
