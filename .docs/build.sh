#!/bin/sh

# Imports utils to build the docs
. ../doc/kit.sh

folders=("dawning")

map_all() {
        local input_folder="$1"

        for file in "$input_folder"/*.md; do
                if [ -f "$file" ]; then
                        echo
                fi
        done
}

doc_folder() {
        local input_folder="$1"
        local output_folder="${2:-dist}"

        mkdir -p "$output_folder"

        for file in "$input_folder"/*.md; do
                if [ -f "$file" ]; then
                        local filename=$(basename "$file" .md)
                        cp template.html "$output_folder/$filename.html"

                        local content=$(doc "$file")
                        template_replace "<meta template_body>" "$output_folder/$filename.html" "$content"
                fi
        done
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

        doc_folder "dawning" "dist/dawning"
}

build

# check if "watch" argument is passed
if [ "$1" = "watch" ]; then
        while inotifywait -e modify -r .; do
                build
                echo "Rebuilt documentation."
        done
fi
