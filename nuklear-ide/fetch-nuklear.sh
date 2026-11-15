#!/bin/sh
# Download the Nuklear headers used by nuklear-ide.
#
# The headers are committed in _import/ so the example builds offline.  Run
# this script when you deliberately want to refresh them; it downloads
# nuklear.h and the GLFW/OpenGL2 backend from the Immediate-Mode-UI/Nuklear
# repository.
#
#   ./fetch-nuklear.sh
#   NK_REF=master ./fetch-nuklear.sh       # latest master instead of the pin
#   NK_DEST=/tmp/nuklear ./fetch-nuklear.sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REF=${NK_REF:-bab18497df988c171fd0d82baad798e1e0e34259}
DEST=${NK_DEST:-$HERE/_import}
BASE="https://raw.githubusercontent.com/Immediate-Mode-UI/Nuklear/$REF"

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

mkdir -p "$DEST"

if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL --retry 3 -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -q -O "$2" "$1"; }
else
    die "curl or wget is required"
fi

download() {
    url=$1
    output=$2
    marker=$3
    tmp="$DEST/.$output.$$"

    trap 'rm -f "$tmp"' EXIT HUP INT TERM
    printf 'GET %s\n' "$url"
    fetch "$url" "$tmp" || die "download failed: $url"
    [ -s "$tmp" ] || die "empty download: $url"
    grep -q "$marker" "$tmp" ||
        die "$output does not contain the expected marker $marker"
    mv -f "$tmp" "$DEST/$output"
    trap - EXIT HUP INT TERM
}

download "$BASE/nuklear.h" \
         "nuklear.h" "NK_NUKLEAR_H_"
download "$BASE/demo/glfw_opengl2/nuklear_glfw_gl2.h" \
         "nuklear_glfw_gl2.h" "NK_GLFW_GL2_H_"

printf 'Nuklear headers updated in %s (ref %s)\n' "$DEST" "$REF"
