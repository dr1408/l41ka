#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 4 ] || [ "$#" -gt 6 ]; then
    printf 'usage: %s <input> <append> <output> <alignment> [append-min-size] [metadata]\n' "$0" >&2
    exit 2
fi

input=$1
append=$2
output=$3
alignment=$4
append_min_size=${5:-0}
metadata=${6:-0}

if (( alignment <= 0 )); then
    printf 'invalid alignment: %s\n' "$alignment" >&2
    exit 2
fi
if (( append_min_size < 0 )); then
    printf 'invalid append minimum size: %s\n' "$append_min_size" >&2
    exit 2
fi

mkdir -p "$(dirname "$output")"
cp "$input" "$output"

size=$(wc -c < "$output")
metadata_size=0
if [ "$metadata" != 0 ]; then
    metadata_size=16
fi

padding=$(( (alignment - ((size + metadata_size) % alignment)) % alignment ))

if [ "$padding" -ne 0 ]; then
    dd if=/dev/zero bs=1 count="$padding" >> "$output" 2>/dev/null
fi

append_size=$(wc -c < "$append")
if [ "$metadata" != 0 ]; then
    printf 'L4KAPNG1' >> "$output"
    for shift in 0 8 16 24 32 40 48 56; do
        byte=$(( (append_size >> shift) & 0xff ))
        printf "\\$(printf '%03o' "$byte")" >> "$output"
    done
fi

cat "$append" >> "$output"

append_padding=$(( append_min_size > append_size ? append_min_size - append_size : 0 ))
if [ "$append_padding" -ne 0 ]; then
    dd if=/dev/zero bs=1 count="$append_padding" >> "$output" 2>/dev/null
fi
