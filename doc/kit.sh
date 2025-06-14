#!/bin/bash
#
#       Dawning Doc Kit (Optimized)
#       Dawn Larsson (dawning.dev) - 2025 - Apache License 2.0
#
source ../utils.sh

inline_format() {
        local text="$1"

        # Images: ![alt](src) -> <img src="src" alt="alt"/>
        while [[ "$text" == *'!['*']('*')'* ]]; do
                local before="${text%%'!['*}"
                local rest="${text#*'!['}"
                local alt="${rest%%']('*}"
                local after="${rest#*']('}"
                local src="${after%%')'*}"
                local end="${after#*')'}"
                text="$before<img src=\"$src\" alt=\"$alt\"/>$end"
        done

        # Code: `code` -> <code>code</code>
        while [[ "$text" == *'`'*'`'* ]]; do
                local before="${text%%'`'*}"
                local rest="${text#*'`'}"
                local code="${rest%%'`'*}"
                local after="${rest#*'`'}"
                text="$before<code>$code</code>$after"
        done

        # Bold: **text** -> <strong>text</strong>
        while [[ "$text" == *'**'*'**'* ]]; do
                local before="${text%%'**'*}"
                local rest="${text#*'**'}"
                local bold="${rest%%'**'*}"
                local after="${rest#*'**'}"
                text="$before<strong>$bold</strong>$after"
        done

        # Links: [text](url) -> <a href="url">text</a>
        while [[ "$text" == *'['*']('*')'* ]]; do
                local before="${text%%'['*}"
                local rest="${text#*'['}"
                local link_text="${rest%%']('*}"
                local after="${rest#*']('}"
                local url="${after%%')'*}"
                local end="${after#*')'}"
                text="$before<a href=\"$url\">$link_text</a>$end"
        done

        # italic: *text* -> <em>text</em>
        while [[ "$text" == *'*'*'*'* ]] && [[ "$text" != *'**'* ]]; do
                local before="${text%%'*'*}"
                local rest="${text#*'*'}"
                local italic="${rest%%'*'*}"
                local after="${rest#*'*'}"

                if [[ "$before" != *'*' ]] && [[ "$after" != '*'* ]]; then
                        text="$before<em>$italic</em>$after"
                else
                        break
                fi
        done

        printf '%s' "$text"
}

doc() {
        local input_file="$1"
        local in_code=false
        local in_list=false
        local line processed

        if [[ -r "$input_file" ]]; then
                while IFS= read -r line || [[ -n "$line" ]]; do

                        # Code blocks
                        if [[ "$line" == '```'* ]]; then
                                if $in_code; then
                                        printf '</code></pre>'
                                        in_code=false
                                else
                                        $in_list && {
                                                printf '</ul>'
                                                in_list=false
                                        }
                                        local lang="${line#'```'}"
                                        lang="${lang// /}" # Remove spaces
                                        if [[ -n "$lang" ]]; then
                                                printf '<pre><code code-%s>' "$lang"
                                        else
                                                printf '<pre><code>'
                                        fi
                                        in_code=true
                                fi
                                continue
                        fi

                        # Inside code block
                        if $in_code; then
                                printf '%s\n' "$line"
                                continue
                        fi

                        # Headers
                        if [[ "$line" =~ ^(#{1,6})[[:space:]]*(.+)$ ]]; then
                                $in_list && {
                                        printf '</ul>'
                                        in_list=false
                                }
                                local level=${#BASH_REMATCH[1]}
                                local text="${BASH_REMATCH[2]}"
                                printf '<h%d>%s</h%d>' "$level" "$text" "$level"
                                continue
                        fi

                        # Lists
                        if [[ "$line" =~ ^[-*+][[:space:]]+(.+)$ ]]; then
                                $in_list || {
                                        printf '<ul>'
                                        in_list=true
                                }
                                processed=$(inline_format "${BASH_REMATCH[1]}")
                                printf '<li>%s</li>' "$processed"
                                continue
                        fi

                        # Empty lines end lists
                        if [[ -z "${line// /}" ]]; then # Empty or whitespace only
                                $in_list && {
                                        printf '</ul>'
                                        in_list=false
                                }
                                continue
                        fi

                        # Regular paragraphs
                        $in_list && {
                                printf '</ul>'
                                in_list=false
                        }
                        processed=$(inline_format "$line")

                        # Images get special treatment
                        if [[ "$processed" =~ ^[[:space:]]*\<img[[:space:]] ]]; then
                                printf '%s' "$processed"
                        else
                                printf '<p>%s</p>' "$processed"
                        fi

                done <"$input_file"

                # Cleanup
                $in_code && printf '</code></pre>'
                $in_list && printf '</ul>'
        fi
}

html_tag() {
        if [ -n "$3" ]; then
                printf "<$1 $3>$2</$1>"
        else
                printf "<$1>$2</$1>"
        fi
}

html_tag_closed() {
        if [ -n "$2" ]; then
                printf "<$1 $2 />"
        else
                printf "<$1 />"
        fi
}

html() {
        local title="$1"
        local content_generator="$2"
        local style_file="$3"

        printf "<!DOCTYPE html><html lang=en><head>"
        html_tag "title" "$title"
        html_tag_closed "meta" 'charset="utf-8"'
        html_tag_closed "meta" 'name=viewport content="width=device-width,initial-scale=1.0"'

        if [ -n "$style_file" ] && [ -f "$style_file" ]; then
                printf "<style>"
                cat "$style_file"
                printf "</style>"
        fi

        printf "</head><body>"

        $content_generator

        printf "</body></html>"
}

# Performs basic minification of CSS files
# Usage: less_css "style/*.css"
less_css() {

        # alt with cat: css=$(cat style/*.css)
        css=""
        for file in $1; do
                [ -f "$file" ] && css+="$(<"$file")"
        done

        start_size=${#css}

        minified=""
        while IFS= read -r line; do

                # Remove comments
                while [[ "$line" == *"/*"* ]]; do
                        if [[ "$line" == *"*/"* ]]; then
                                before="${line%%/*}"
                                after="${line#*\*/}"
                                line="$before$after"
                        else # Multi-lines
                                line="${line%%/*}"
                                break
                        fi
                done

                # Remove leading whitespace
                line="${line#"${line%%[![:space:]]*}"}"

                # Remove trailing whitespace
                line="${line%"${line##*[![:space:]]}"}"

                # Skip empty lines
                [ -n "$line" ] && minified="$minified$line"
        done <<<"$css"

        # Remove spaces around colons
        minified="${minified//: /:}"

        # Remove spaces around semicolons
        minified="${minified//; /;}"

        # Remove spaces around commas
        minified="${minified//, /,}"

        # Remove spaces around opening braces
        minified="${minified// {/{}"

        # Remove spaces around greater-than selectors
        minified="${minified// > />}"

        # Remove spaces around plus selectors
        minified="${minified// + /+}"

        printf '%s' "$minified" >dist/style.css

        size_diff "$start_size" "${#minified}" "CSS"
}
