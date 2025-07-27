#!/bin/bash
#
#       Dawning Doc Kit (Optimized)
#       Dawn Larsson (dawning.dev) - 2025 - Apache License 2.0
#
. "${KIT_DIR:-./dawning-kit}/utils.sh"

md_inline_format() {
        local text="$1"

        # Handle line breaks
        text="${text//\\\\n/<br>}"
        text="${text//  $'\n'/<br>}"

        # Images: ![alt](src) -> <img src="src" alt="alt"/>
        while [[ "$text" = *'!['*']('*')'* ]]; do
                local before="${text%%'!['*}"
                local rest="${text#*'!['}"
                local alt="${rest%%']('*}"
                local after="${rest#*']('}"
                local src="${after%%')'*}"
                local end="${after#*')'}"
                text="$before<img src=\"$src\" alt=\"$alt\"/>$end"
        done

        # Code: `code` -> <code>code</code>
        while [[ "$text" = *'`'*'`'* ]]; do
                local before="${text%%'`'*}"
                local rest="${text#*'`'}"
                local code="${rest%%'`'*}"
                local after="${rest#*'`'}"
                text="$before<code>$code</code>$after"
        done

        # Bold: **text** -> <strong>text</strong>
        while [[ "$text" = *'**'*'**'* ]]; do
                local before="${text%%'**'*}"
                local rest="${text#*'**'}"
                local bold="${rest%%'**'*}"
                local after="${rest#*'**'}"
                text="$before<strong>$bold</strong>$after"
        done

        # Links: [text](url) -> <a href="url">text</a>
        while [[ "$text" = *'['*']('*')'* ]]; do
                local before="${text%%'['*}"
                local rest="${text#*'['}"
                local link_text="${rest%%']('*}"
                local after="${rest#*']('}"
                local url="${after%%')'*}"
                local end="${after#*')'}"
                text="$before<a href=\"$url\">$link_text</a>$end"
        done

        # italic: *text* -> <em>text</em>
        while [[ "$text" = *'*'*'*'* ]] && [[ "$text" != *'**'* ]]; do
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

        # Strikethrough: ~~text~~ -> <del>text</del>
        while [[ "$text" = *'~~'*'~~'* ]]; do
                local before="${text%%'~~'*}"
                local rest="${text#*'~~'}"
                local strike="${rest%%'~~'*}"
                local after="${rest#*'~~'}"
                text="$before<del>$strike</del>$after"
        done

        printf '%s' "$text"
}
doc() {
        local input_file="$1"
        local in_code=false
        local in_list=false
        local in_quote=false
        local paragraph_buffer=""
        local line processed

        if [[ -r "$input_file" ]]; then
                while IFS= read -r line || [[ -n "$line" ]]; do

                        # Code blocks
                        if [[ "$line" = '```'* ]]; then
                                # Flush paragraph buffer
                                if [[ -n "$paragraph_buffer" ]]; then
                                        processed=$(md_inline_format "$paragraph_buffer")
                                        printf '<p>%s</p>' "$processed"
                                        paragraph_buffer=""
                                fi
                                
                                if $in_code; then
                                        printf '</code></pre>'
                                        in_code=false
                                else
                                        $in_list && { printf '</ul>'; in_list=false; }
                                        $in_quote && { printf '</blockquote>'; in_quote=false; }
                                        local lang="${line#'```'}"
                                        lang="${lang// /}"
                                        if [ -n "$lang" ]; then
                                                printf '<pre><code class="language-%s">' "$lang"
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

                        # Horizontal rules
                        if [[ "$line" =~ ^([-*_]){3,}[[:space:]]*$ ]]; then
                                # Flush paragraph buffer
                                if [[ -n "$paragraph_buffer" ]]; then
                                        processed=$(md_inline_format "$paragraph_buffer")
                                        printf '<p>%s</p>' "$processed"
                                        paragraph_buffer=""
                                fi
                                $in_list && { printf '</ul>'; in_list=false; }
                                $in_quote && { printf '</blockquote>'; in_quote=false; }
                                printf '<hr>'
                                continue
                        fi

                        # Headers
                        if [[ "$line" =~ ^(#{1,6})[[:space:]]*(.+)$ ]]; then
                                # Flush paragraph buffer
                                if [[ -n "$paragraph_buffer" ]]; then
                                        processed=$(md_inline_format "$paragraph_buffer")
                                        printf '<p>%s</p>' "$processed"
                                        paragraph_buffer=""
                                fi
                                
                                $in_list && { printf '</ul>'; in_list=false; }
                                $in_quote && { printf '</blockquote>'; in_quote=false; }
                                local level=${#BASH_REMATCH[1]}
                                local text="${BASH_REMATCH[2]}"
                                processed=$(md_inline_format "$text")
                                printf '<h%d>%s</h%d>' "$level" "$processed" "$level"
                                continue
                        fi

                        # Blockquotes
                        if [[ "$line" =~ ^">"[[:space:]]*(.*)$ ]]; then
                                # Flush paragraph buffer
                                if [[ -n "$paragraph_buffer" ]]; then
                                        processed=$(md_inline_format "$paragraph_buffer")
                                        printf '<p>%s</p>' "$processed"
                                        paragraph_buffer=""
                                fi
                                
                                $in_list && { printf '</ul>'; in_list=false; }
                                $in_quote || { printf '<blockquote>'; in_quote=true; }
                                local quote_text="${BASH_REMATCH[1]}"
                                if [[ -n "$quote_text" ]]; then
                                        processed=$(md_inline_format "$quote_text")
                                        printf '<p>%s</p>' "$processed"
                                fi
                                continue
                        fi

                        # Lists
                        if [[ "$line" =~ ^[-*+][[:space:]]+(.+)$ ]]; then
                                # Flush paragraph buffer
                                if [[ -n "$paragraph_buffer" ]]; then
                                        processed=$(md_inline_format "$paragraph_buffer")
                                        printf '<p>%s</p>' "$processed"
                                        paragraph_buffer=""
                                fi
                                
                                $in_quote && { printf '</blockquote>'; in_quote=false; }
                                $in_list || { printf '<ul>'; in_list=true; }
                                processed=$(md_inline_format "${BASH_REMATCH[1]}")
                                printf '<li>%s</li>' "$processed"
                                continue
                        fi

                        # Empty lines
                        if [[ -z "${line// /}" ]]; then
                                if [[ -n "$paragraph_buffer" ]]; then
                                        processed=$(md_inline_format "$paragraph_buffer")
                                        printf '<p>%s</p>' "$processed"
                                        paragraph_buffer=""
                                fi
                                $in_list && { printf '</ul>'; in_list=false; }
                                $in_quote && { printf '</blockquote>'; in_quote=false; }
                                continue
                        fi

                        # HTML tags
                        if [[ "$line" =~ ^[[:space:]]*\<[[:alpha:]][[:alnum:]\-]* ]]; then
                                # Flush paragraph buffer
                                if [[ -n "$paragraph_buffer" ]]; then
                                        processed=$(md_inline_format "$paragraph_buffer")
                                        printf '<p>%s</p>' "$processed"
                                        paragraph_buffer=""
                                fi
                                printf '%s\n' "$line"
                                continue
                        fi

                        # add to paragraph buffer
                        $in_list && { printf '</ul>'; in_list=false; }
                        $in_quote && { printf '</blockquote>'; in_quote=false; }
                        
                        if [[ -n "$paragraph_buffer" ]]; then
                                paragraph_buffer="$paragraph_buffer $line"
                        else
                                paragraph_buffer="$line"
                        fi

                done <"$input_file"

                if [[ -n "$paragraph_buffer" ]]; then
                        processed=$(md_inline_format "$paragraph_buffer")
                        printf '<p>%s</p>' "$processed"
                fi
                $in_code && printf '</code></pre>'
                $in_list && printf '</ul>'
                $in_quote && printf '</blockquote>'
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
# Usage: less_css "style/*.css" dist/style.css
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
                while [[ "$line" = *"/*"* ]]; do
                        if [[ "$line" = *"*/"* ]]; then
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

        # Remove semicolon before closing brace
        minified="${minified//;\}/\}}"

        # Remove space before !important
        minified="${minified// !important/!important}"

        # Remove single quotes from attribute selectors like [attr='value'] -> [attr=value]
        # This is safe for simple values like 'button', 'reset', 'submit'
        minified="${minified//=\'/=}"  # Replace =' with =
        minified="${minified//\'\]/]}" # Replace '] with ]

        # Remove leading zeros from decimal values
        minified="${minified//0\./.}"

        minified="${minified//url(\"/url(}"
        minified="${minified//\")/)}"
        minified="${minified//url(\'/url(}"
        minified="${minified//\')/)}"

        minified="${minified//font-weight:bolder/font-weight:900}"
        minified="${minified//font-weight:bold/font-weight:700}"
        minified="${minified//font-weight:normal/font-weight:400}"

        # #00000000 -> #0000
        minified="${minified//00000000/0000}"

        # Remove double quotes around values that don't contain spaces
        processed_minified=""
        temp_minified="$minified"
        while [[ "$temp_minified" = *\"* ]]; do
                before_first_quote="${temp_minified%%\"*}"
                processed_minified+="$before_first_quote"
                rest_after_first_quote="${temp_minified#*\"}"

                if [[ "$rest_after_first_quote" = *\"* ]]; then
                        content_in_quotes="${rest_after_first_quote%%\"*}"
                        after_closing_quote="${rest_after_first_quote#*\"}"

                        if [[ "$content_in_quotes" != *" "* && "$content_in_quotes" != "" ]]; then
                                processed_minified+="$content_in_quotes"
                        else
                                processed_minified+="\"$content_in_quotes\""
                        fi
                        temp_minified="$after_closing_quote"
                else
                        processed_minified+="\"$rest_after_first_quote"
                        temp_minified=""
                fi
        done
        processed_minified+="$temp_minified"
        minified="$processed_minified"

        printf '%s' "$minified" >$2

        size_diff "$start_size" "${#minified}" "CSS"
}

# Replaces <meta template_body> with the content of the second argument file
template_replace() {
        local placeholder="$1"
        local template_file="$2"
        local content="$3"
        local template_content
        local original_template_content

        # Read the entire template file into a variable
        original_template_content=$(<"$template_file")
        template_content="$original_template_content"

        # Perform the replacement in the variable
        template_content="${template_content//$placeholder/$content}"

        # Check if any replacement occurred
        if [ "$template_content" != "$original_template_content" ]; then
                # Write the modified content back to the original file
                printf "%s" "$template_content" >"$template_file"
        else
                echo "Warning: '$placeholder' not found in $template_file" >&2
        fi
}