#!/bin/sh
# snap.sh <windows path to plugin> <name>  -> build/snap/<name>.png (+ .txt metrics)
# Renders the window through the DLL's Snapshot entry point, without installing.
here='C:/projects/shell-plugin-properties'
mkdir -p "$here/build/snap"
rundll32.exe "$here/build/bin/PluginProperties.dll,Snapshot" "$1" "$here/build/snap/$2.png"
