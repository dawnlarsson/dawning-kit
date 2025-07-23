#!/bin/sh

# Imports utils to build the docs
. ../doc/kit.sh

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
