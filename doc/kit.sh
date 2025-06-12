#!/bin/bash
#
#       Dawning Doc Kit (Optimized)
#       Dawn Larsson (dawning.dev) - 2025 - Apache License 2.0
#

html_tag() {
        local tag="$1"
        local content="$2"
        local attrs="$3"

        if [ -n "$attrs" ]; then
                echo "<$tag $attrs>$content</$tag>"
        else
                echo "<$tag>$content</$tag>"
        fi
}

html_tag_closed() {
        local tag="$1"
        local attrs="$2"

        if [ -n "$attrs" ]; then
                echo "<$tag $attrs />"
        else
                echo "<$tag />"
        fi
}

md_inline_all() {
        local line="$1"
        sed -e 's/!\[\([^]]*\)\](\([^)]*\))/<img src="\2" alt="\1" \/>/g' \
                -e 's/`\([^`]*\)`/<code>\1<\/code>/g' \
                -e 's/\*\*\([^*]*\)\*\*/<strong>\1<\/strong>/g' \
                -e 's/\(^\|[^*]\)\*\([^*]\+\)\*\($\|[^*]\)/\1<em>\2<\/em>\3/g' \
                -e 's/\[\([^]]*\)\](\([^)]*\))/<a href="\2">\1<\/a>/g' <<<"$line"
}

md_to_html() {
        local input_file="$1"
        local in_code_block=false
        local in_list=false
        local line
        local processed_line

        while IFS= read -r line; do
                if [[ "$line" == '```'* ]]; then
                        if $in_code_block; then
                                echo "</code></pre>"
                                in_code_block=false
                        else

                                if $in_list; then
                                        echo "</ul>"
                                        in_list=false
                                fi

                                local lang="${line#\`\`\`}"
                                lang="${lang// /}"

                                if [ -n "$lang" ]; then
                                        echo "<pre><code class=\"language-$lang\">"
                                else
                                        echo "<pre><code>"
                                fi
                                in_code_block=true
                        fi
                        continue
                fi

                if $in_code_block; then
                        echo "$line"
                        continue
                fi

                if [[ "$line" == '#'* ]]; then
                        if $in_list; then
                                echo "</ul>"
                                in_list=false
                        fi

                        local temp="${line%%[^#]*}"
                        local level=${#temp}

                        if [ $level -gt 0 ] && [ $level -le 6 ]; then
                                local text="${line#*# }"
                                text="${text# }"
                                html_tag "h$level" "$text"
                        fi
                        continue
                fi

                if [[ "$line" == [-*+]' '* ]]; then
                        if ! $in_list; then
                                echo "<ul>"
                                in_list=true
                        fi

                        local list_text="${line#[-*+] }"
                        list_text=$(md_inline_all "$list_text")
                        html_tag "li" "$list_text"
                        continue
                fi

                if $in_list && [ -n "$line" ]; then
                        echo "</ul>"
                        in_list=false
                fi

                if [ -z "$line" ]; then
                        continue
                fi

                processed_line=$(md_inline_all "$line")

                if [[ "$processed_line" == '<img'* ]] && [[ ! "$processed_line" == *'<'*'<img'* ]]; then
                        echo "$processed_line"
                else
                        html_tag "p" "$processed_line"
                fi

        done <"$input_file"

        if $in_code_block; then
                echo "</code></pre>"
        fi
        if $in_list; then
                echo "</ul>"
        fi
}

html_document() {
        local title="$1"
        local content_generator="$2"
        local style_file="$3"

        echo "<!DOCTYPE html><html lang=en>"

        echo "<head>"
        html_tag "title" "$title"
        html_tag_closed "meta" 'charset="utf-8"'
        html_tag_closed "meta" 'name=viewport content="width=device-width,initial-scale=1.0"'

        if [ -n "$style_file" ] && [ -f "$style_file" ]; then
                echo "<style>"
                cat "$style_file"
                echo "</style>"
        fi

        echo "</head><body>"

        $content_generator

        echo "</body></html>"
}

doc() {
        local output="$1"
        local input_file="$2"
        local title="$3"
        local style_file="$4"

        if [ -z "$title" ]; then
                title=$(basename "$input_file" .md)
        fi

        content_generator() {
                md_to_html "$input_file"
        }

        html_document "$title" content_generator "$style_file" >"$output"
}

doc_batch() {
        local input_dir="$1"
        local output_dir="$2"
        local style_file="$3"

        mkdir -p "$output_dir"

        for md_file in "$input_dir"/*.md; do
                if [ -f "$md_file" ]; then
                        local basename=$(basename "$md_file" .md)
                        local output_file="$output_dir/$basename.html"
                        echo "Processing: $md_file -> $output_file"
                        doc "$output_file" "$md_file" "$basename" "$style_file"
                fi
        done
}
