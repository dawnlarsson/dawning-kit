# usage: size_diff "$start_size" "${#minified}" "CSS"
# returns: "CSS: 123 KB → 45 KB (63% smaller)"
size_diff() {
        local original=$1
        local minified=$2
        local savings=$((original - minified))
        local percentage=$((savings * 100 / original))

        local orig_fmt min_fmt
        if [ "$original" -lt 1024 ]; then
                orig_fmt="${original} B"
        elif [ "$original" -lt 1048576 ]; then
                orig_fmt="$(echo "scale=1; $original/1024" | bc) KB"
        else
                orig_fmt="$(echo "scale=1; $original/1048576" | bc) MB"
        fi

        if [ "$minified" -lt 1024 ]; then
                min_fmt="${minified} B"
        elif [ "$minified" -lt 1048576 ]; then
                min_fmt="$(echo "scale=1; $minified/1024" | bc) KB"
        else
                min_fmt="$(echo "scale=1; $minified/1048576" | bc) MB"
        fi

        printf "$3: %s → %s (%d%% smaller)\n" "$orig_fmt" "$min_fmt" "$percentage"
}
