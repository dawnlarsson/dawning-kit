#!/bin/bash

size_fmt() {
        local size=$1
        local formatted_size

        if [ "$size" -lt 1024 ]; then
                formatted_size="${size} B"
        elif [ "$size" -lt 1048576 ]; then # KB
                local kb_int=$((size / 1024))
                local kb_rem=$((size % 1024))
                local kb_dec=$(((kb_rem * 10) / 1024))
                formatted_size="${kb_int}.${kb_dec} KB"
        else # MB
                local mb_int=$((size / 1048576))
                local mb_rem=$((size % 1048576))
                local mb_dec=$(((mb_rem * 10) / 1048576))
                formatted_size="${mb_int}.${mb_dec} MB"
        fi
        echo "$formatted_size"
}

# usage: size_diff "$start_size" "${#minified}" "CSS"
# returns: "CSS: 123.0 KB → 45.0 KB (63% smaller)"
size_diff() {
        local original=$1
        local minified=$2
        local label=$3
        local savings=$((original - minified))
        local percentage=0
        if [ "$original" -ne 0 ]; then # Avoid division by zero
                percentage=$((savings * 100 / original))
        fi

        local orig_fmt
        local min_fmt

        orig_fmt=$(size_fmt "$original")
        min_fmt=$(size_fmt "$minified")

        printf "%s: %s → %s (%d%% smaller)\\n" "$label" "$orig_fmt" "$min_fmt" "$percentage"
}

# stat takes -c on GNU and -f on BSD/macOS; neither accepts the other's flag.
size_bytes() {
        if stat -c%s "$1" 2>/dev/null; then
                return 0
        fi
        stat -f%z "$1" 2>/dev/null
}

# just prints the file size in bytes, KB, and MB
size() {
        local bytes
        local formatted

        if ! bytes=$(size_bytes "$1") || [ -z "$bytes" ]; then
                echo "size: cannot stat '$1'" >&2
                return 1
        fi

        formatted=$(size_fmt "$bytes")

        echo "$1: $bytes bytes ($formatted)"
}

file() { cat "$1"; }
write() { echo -n "$2" > "$1"; }
append() { echo -n "$2" >> "$1"; }
exists() { [ -f "$1" ]; }
