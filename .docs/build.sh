#!/bin/bash

# ensure we are in .docs directory
cd "$(dirname "$0")" || exit 1

#
# Builds dawning documentation
#
. ../doc/kit.sh

# Replaces <meta template_body> with the content of the second argument file
template_replace() {
        local placeholder="$1"
        local template_file="$2"
        local content="$3"
        local replacement_content
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

build() {
        rm -rf dist
        mkdir -p dist
        cp -r assets/* dist/

        less_css "style/*.css" dist/style.css

        cp template.html dist/index.html

        index=$(doc ../README.md)
        template_replace "<meta template_body>" dist/index.html "$index"

        local side=$(doc side.md)
        template_replace "<meta template_side>" dist/index.html "$side"
}

build

# check if "watch" argument is passed
if [ "$1" = "watch" ]; then
        while inotifywait -e modify -r .; do
                build
                echo "Rebuilt documentation."
        done
fi
