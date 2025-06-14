#!/bin/bash

#
# Builds dawning documentation
#
source ../doc/kit.sh

rm -rf dist
mkdir -p dist
cp -r assets/* dist/

less_css "style/*.css" dist/style.css
