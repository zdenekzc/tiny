# need.sh — shared dependency check for the helper scripts.
#
# Source it after the script has computed its repo-root variable and it
# replaces the per-script need():
#
#     . "$REPO_ROOT/need.sh"
#     need qemu-system-x86_64
#
# On a missing program it dies with the name of the Fedora/Debian package
# that provides it, so the fix is one copy-paste command away.

# program -> Fedora package (dnf)
declare -A _NEED_PKG_FEDORA=(
    [unzip]=unzip
    [7z]=p7zip-plugins
    [awk]=gawk
    [curl]=curl
    [du]=coreutils
    [fallocate]=util-linux
    [gcc]=gcc
    [genisoimage]=genisoimage
    [git]=git
    [gzip]=gzip
    [isoinfo]=genisoimage
    [mcopy]=mtools
    [mmd]=mtools
    [mkfs.fat]=dosfstools
    [python3]=python3
    [qemu-system-aarch64]=qemu-system-aarch64
    [qemu-system-x86_64]=qemu-system-x86
    [sort]=coreutils
    [tar]=tar
)

# program -> Debian/Ubuntu package (apt)
declare -A _NEED_PKG_DEBIAN=(
    [unzip]=unzip
    [7z]=p7zip-full
    [awk]=gawk
    [curl]=curl
    [du]=coreutils
    [fallocate]=util-linux
    [gcc]=gcc
    [genisoimage]=genisoimage
    [git]=git
    [gzip]=gzip
    [isoinfo]=genisoimage
    [mcopy]=mtools
    [mmd]=mtools
    [mkfs.fat]=dosfstools
    [python3]=python3
    [qemu-system-aarch64]=qemu-system-aarch64
    [qemu-system-x86_64]=qemu-system-x86
    [sort]=coreutils
    [tar]=tar
)

need() {
    command -v "$1" >/dev/null 2>&1 && return 0
    local pkg_map=""
    if command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; then
        pkg_map=_NEED_PKG_FEDORA
    elif command -v apt-get >/dev/null 2>&1; then
        pkg_map=_NEED_PKG_DEBIAN
    fi
    local pkg=""
    if [[ -n $pkg_map ]]; then
        eval "pkg=\${${pkg_map}[$1]-}"
    fi
    if [[ -n $pkg ]]; then
        echo "error: missing program '$1' — install it with: sudo dnf install $pkg" >&2
    else
        echo "error: missing program '$1'" >&2
    fi
    exit 1
}
