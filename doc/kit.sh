#!/bin/bash
#
#       Dawning Doc Kit
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

html_raw() {
        echo "$1"
}

md_heading() {
        local line="$1"
        local level=$(echo "$line" | sed 's/^\(#*\).*/\1/' | wc -c)
        level=$((level - 1))

        if [ $level -gt 0 ] && [ $level -le 6 ]; then
                local text=$(echo "$line" | sed 's/^#* *//')
                html_tag "h$level" "$text"
                return 0
        fi
        return 1
}

md_paragraph() {
        local line="$1"
        if [ -n "$line" ]; then
                html_tag "p" "$line"
                return 0
        fi
        return 1
}

md_code_block() {
        local line="$1"
        # Check for code block markers
        if echo "$line" | grep -q '^```'; then
                # Extract language if present (anything after ```)
                local lang=$(echo "$line" | sed 's/^```//' | tr -d '[:space:]')
                if [ -n "$lang" ]; then
                        echo "<pre><code class=\"language-$lang\">"
                else
                        echo "<pre><code>"
                fi
                return 0
        fi
        return 1
}

md_list_item() {
        local line="$1"
        if echo "$line" | grep -q '^[*+-] '; then
                local text=$(echo "$line" | sed 's/^[*+-] *//')
                echo "$text"
                return 0
        fi
        return 1
}

md_inline_code() {
        local line="$1"
        echo "$line" | sed 's/`\([^`]*\)`/<code>\1<\/code>/g'
}

md_inline_bold() {
        local line="$1"
        # Handle bold markers, even with special characters inside
        echo "$line" | sed 's/\*\*\([^*]*\)\*\*/<strong>\1<\/strong>/g'
}

md_inline_italic() {
        local line="$1"
        # Avoid matching bold markers by ensuring single asterisks
        echo "$line" | sed 's/\(^\|[^*]\)\*\([^*]\+\)\*\($\|[^*]\)/\1<em>\2<\/em>\3/g'
}

md_inline_link() {
        local line="$1"
        echo "$line" | sed 's/\[\([^]]*\)\](\([^)]*\))/<a href="\2">\1<\/a>/g'
}

md_inline_image() {
        local line="$1"
        echo "$line" | sed 's/!\[\([^]]*\)\](\([^)]*\))/<img src="\2" alt="\1" \/>/g'
}

md_inline() {
        local line="$1"
        # Process images first (before links, since they have similar syntax)
        line=$(md_inline_image "$line")
        # Process inline code to protect backticks
        line=$(md_inline_code "$line")
        # Then process bold
        line=$(md_inline_bold "$line")
        # Then italic (careful not to match bold markers)
        line=$(md_inline_italic "$line")
        # Finally links
        line=$(md_inline_link "$line")
        echo "$line"
}

md_to_html() {
        local input_file="$1"
        local in_code_block=false
        local in_list=false

        while IFS= read -r line; do
                # Handle code blocks
                if echo "$line" | grep -q '^```'; then
                        if $in_code_block; then
                                # Closing code block
                                echo "</code></pre>"
                                in_code_block=false
                        else
                                # Opening code block
                                # Close any open list first
                                if $in_list; then
                                        echo "</ul>"
                                        in_list=false
                                fi

                                # Extract language if present
                                local lang=$(echo "$line" | sed 's/^```//' | tr -d '[:space:]')
                                if [ -n "$lang" ]; then
                                        echo "<pre><code class=\"language-$lang\">"
                                else
                                        echo "<pre><code>"
                                fi
                                in_code_block=true
                        fi
                        continue
                fi

                # If in code block, output line as-is
                if $in_code_block; then
                        echo "$line"
                        continue
                fi

                # Check if this is a heading FIRST, before outputting
                if echo "$line" | grep -q '^#'; then
                        # Close any open list BEFORE the heading
                        if $in_list; then
                                echo "</ul>"
                                in_list=false
                        fi
                        # Now process the heading
                        md_heading "$line"
                        continue
                fi

                # Check if this is a list item
                if echo "$line" | grep -q '^[*+-] '; then
                        # Start list if not already in one
                        if ! $in_list; then
                                echo "<ul>"
                                in_list=true
                        fi

                        # Extract and process the list item text
                        local list_text=$(echo "$line" | sed 's/^[*+-] *//')
                        list_text=$(md_inline "$list_text")
                        html_tag "li" "$list_text"
                        continue
                fi

                # If we were in a list and this line doesn't start with a list marker
                if $in_list && [ -n "$line" ]; then
                        echo "</ul>"
                        in_list=false
                fi

                # Skip empty lines
                if [ -z "$line" ]; then
                        continue
                fi

                # Check if line starts with an image
                if echo "$line" | grep -q '^!'; then
                        # Process the entire line for images
                        line=$(md_inline "$line")
                        # Don't wrap in paragraph if it's just an image
                        if echo "$line" | grep -q '^<img'; then
                                echo "$line"
                        else
                                html_tag "p" "$line"
                        fi
                else
                        # Process as paragraph
                        line=$(md_inline "$line")
                        html_tag "p" "$line"
                fi

        done <"$input_file"

        # Close any open structures at end of file
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
