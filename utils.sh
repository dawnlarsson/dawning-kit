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

# just prints the file size in bytes, KB, and MB
size() {
        local size
        local size_fmt
        
        size=$(stat -c%s "$1")
        size_fmt=$(size_fmt "$size")
        
        echo "$1: $size bytes ($size_fmt)"
}

file() { cat "$1"; }
write() { echo -n "$2" > "$1"; }
append() { echo -n "$2" >> "$1"; }
exists() { [ -f "$1" ]; }

file_itter() {
        local index=0
        while IFS= read -r line || [ -n "$line" ]; do
                "$2" "$line" "$index"
                index=$((index + 1))
        done < "$1"
}

label_finder() {
        local match="$1"
        local file="$2"
        local callback="$3"
        
        local found=0
        local depth=0
        local index=0
        local blob=""
        local use_braces=0
        local base_indent=0
        local checking_next_line=0
        
        while IFS= read -r line || [ -n "$line" ]; do
                if [ $found -eq 0 ]; then
                        # Check if line starts with match
                        if [[ "$line" =~ ^"$match" ]]; then
                                found=1
                                blob="$line"
                                
                                # Check if opening brace is on this line
                                if [[ "$line" =~ \{ ]]; then
                                        use_braces=1
                                        depth=$(( depth + $(grep -o '{' <<< "$line" | wc -l) ))
                                        depth=$(( depth - $(grep -o '}' <<< "$line" | wc -l) ))
                                        
                                        [ -n "$callback" ] && "$callback" "$line" "$index"
                                        index=$((index + 1))
                                        
                                        # Check if already balanced (single-line function)
                                        [ $depth -eq 0 ] && break
                                else
                                        # Need to check next line for opening brace
                                        checking_next_line=1
                                        base_indent=$(echo "$line" | sed 's/[^ \t].*//' | wc -c)
                                        base_indent=$((base_indent - 1))
                                        
                                        [ -n "$callback" ] && "$callback" "$line" "$index"
                                        index=$((index + 1))
                                fi
                        fi
                elif [ $checking_next_line -eq 1 ]; then
                        # Check if this line starts with opening brace
                        blob="$blob"$'\n'"$line"
                        
                        if [[ "$line" =~ ^[[:space:]]*\{ ]]; then
                                use_braces=1
                                checking_next_line=0
                                depth=$(( depth + $(grep -o '{' <<< "$line" | wc -l) ))
                                depth=$(( depth - $(grep -o '}' <<< "$line" | wc -l) ))
                        else
                                # No brace found, must be indent-based
                                use_braces=0
                                checking_next_line=0
                        fi
                        
                        [ -n "$callback" ] && "$callback" "$line" "$index"
                        index=$((index + 1))
                else
                        if [ $use_braces -eq 1 ]; then
                                # Brace-based language
                                blob="$blob"$'\n'"$line"
                                
                                depth=$(( depth + $(grep -o '{' <<< "$line" | wc -l) ))
                                depth=$(( depth - $(grep -o '}' <<< "$line" | wc -l) ))
                                
                                [ -n "$callback" ] && "$callback" "$line" "$index"
                                index=$((index + 1))
                                
                                [ $depth -eq 0 ] && break
                        else
                                # Indent-based language
                                if [[ "$line" =~ ^[[:space:]]*$ ]]; then
                                        blob="$blob"$'\n'"$line"
                                        [ -n "$callback" ] && "$callback" "$line" "$index"
                                        index=$((index + 1))
                                else
                                        local curr_indent=$(echo "$line" | sed 's/[^ \t].*//' | wc -c)
                                        curr_indent=$((curr_indent - 1))
                                        
                                        if [ $curr_indent -gt $base_indent ]; then
                                                blob="$blob"$'\n'"$line"
                                                [ -n "$callback" ] && "$callback" "$line" "$index"
                                                index=$((index + 1))
                                        else
                                                break
                                        fi
                                fi
                        fi
                fi
        done < "$file"

        if [ "$4" = true ]; then
                echo "$blob"
        fi
}