#!/bin/bash
# fetch-gcc-local.sh — saves the ToaruOS cross toolchain from the Docker image
# docker.io/toaruos/build-tools:1.99.x as _import/gcc_local.tgz (x86_64), or
# from the :aarch64 tag as _import/gcc_arm64.tgz (-a).
#
#   ./scripts/fetch-gcc-local.sh          pull the image if needed, write _import/gcc_local.tgz
#   ./scripts/fetch-gcc-local.sh -a       arm64 variant, write _import/gcc_arm64.tgz
#   ./scripts/fetch-gcc-local.sh -f       pull again and overwrite an existing archive
#   ./scripts/fetch-gcc-local.sh -n       no pull; require the image in the local store
#   ./scripts/fetch-gcc-local.sh -r       run docker/podman through sudo
#   ./scripts/fetch-gcc-local.sh -o FILE  write the archive elsewhere
#
# The default image (https://github.com/klange/toaru-build-tools) is an Ubuntu
# xenial with the gcc 10.3.0 x86_64-pc-toaru cross toolchain in /root/gcc_local;
# -a uses the :aarch64 tag, which has aarch64-unknown-toaru in the same place
# (the image itself is still linux/amd64, so PLATFORM does not change).  The
# script exports the container filesystem (the same thing as `docker export`)
# and repacks just root/gcc_local; nothing from the image is executed.
# Both archives unpack as gcc_local/; the x86_64 one is what
# build-linux-toaruos.sh downloads from kmlinux.fjfi.cvut.cz:
#   mkdir -p _library && tar xzf _import/gcc_local.tgz -C _library
#
# Docker is preferred when both are installed; Podman works too.
# ENGINE=docker|podman forces one.  The image is amd64 only, also on other hosts.
#
# Rootless Podman, what it needs (Fedora paths; all but the first are usually
# already in place):
#
#   * /etc/subuid and /etc/subgid entries for the user, e.g.
#       sudo usermod --add-subuids 100000-165535 --add-subgids 100000-165535 "$USER"
#     The range must not overlap another user's.  Without it podman can map
#     only uid/gid 0, and pulling any image with files owned by other IDs
#     (this one has /etc/gshadow = gid 42) fails with "potentially insufficient
#     UIDs or GIDs available in user namespace".
#   * newuidmap/newgidmap from shadow-utils, with setuid root or the file
#     capabilities cap_setuid=ep / cap_setgid=ep (check with getcap).
#   * unprivileged user namespaces: user.max_user_namespaces > 0
#     (sysctl user.max_user_namespaces).  kernel.unprivileged_userns_clone is
#     the Debian/Ubuntu name; it does not exist on Fedora.
#   * after changing the subuid/subgid files: podman system migrate (as the
#     user; it stops running rootless containers, so do it when none run).
#
# Check the result with:
#   podman unshare cat /proc/self/uid_map     # at least two lines
# Until then use -r, i.e. the root Podman storage.
set -euo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PLATFORM=${PLATFORM:-linux/amd64}

force=0
no_pull=0
use_sudo=0
variant=x86_64

usage() {
    cat <<'EOF'
usage: fetch-gcc-local.sh [-a] [-f] [-n] [-r] [-o FILE]
  -a   fetch the arm64 (aarch64-unknown-toaru) toolchain, image tag :aarch64
  -f   pull the image again and overwrite an existing archive
  -n   do not pull; the image must already be in the local store
  -r   run docker/podman through sudo (needed for rootless podman without subuid)
  -o   output archive (default: _import/gcc_local.tgz, -a: _import/gcc_arm64.tgz)
EOF
}

die() { echo "error: $*" >&2; exit 1; }

while getopts 'afnro:h' opt; do
    case $opt in
        a) variant=aarch64 ;;
        f) force=1 ;;
        n) no_pull=1 ;;
        r) use_sudo=1 ;;
        o) OUT=$OPTARG ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

if (( force && no_pull )); then
    die "-f and -n are mutually exclusive"
fi

case $variant in
    x86_64)
        IMAGE=${IMAGE:-docker.io/toaruos/build-tools:1.99.x}
        OUT=${OUT:-$DIR/_import/gcc_local.tgz}
        cc=x86_64-pc-toaru-gcc
        ;;
    aarch64)
        IMAGE=${IMAGE:-docker.io/toaruos/build-tools:aarch64}
        OUT=${OUT:-$DIR/_import/gcc_arm64.tgz}
        cc=aarch64-unknown-toaru-gcc
        ;;
esac

if [[ -e $OUT && $force -eq 0 ]]; then
    echo "$OUT already exists (use -f to rebuild)"
    exit 0
fi

pull=auto        # auto | always | never
(( force )) && pull=always
(( no_pull )) && pull=never

# ---------------------------------------------------------------- 1. engine
prefix=()
(( use_sudo )) && prefix=(sudo)

if [[ -n ${ENGINE:-} ]]; then
    candidates=("$ENGINE")
else
    candidates=(docker podman)
fi

engine=()
for e in "${candidates[@]}"; do
    command -v "$e" > /dev/null 2>&1 || continue
    if ! "${prefix[@]}" "$e" info > /dev/null 2>&1; then
        continue
    fi
    engine=("${prefix[@]}" "$e")
    break
done
[[ ${#engine[@]} -gt 0 ]] || die "no usable container engine (tried: ${candidates[*]})"
echo "using: ${engine[*]}"

# Rootless podman with only uid 0 mapped cannot unpack files owned by other
# users/groups (this image has /etc/gshadow = :42).  Fail early with a fix,
# not with podman's wall of text in the middle of the pull.
if [[ ${engine[*]} == podman ]]; then
    umap=$("${engine[@]}" info --format '{{len .Host.IDMappings.UIDMap}}' 2> /dev/null || echo 0)
    gmap=$("${engine[@]}" info --format '{{len .Host.IDMappings.GIDMap}}' 2> /dev/null || echo 0)
    if (( umap < 2 || gmap < 2 )); then
        die "rootless podman maps only uid/gid 0 for $USER (no subuid/subgid range).
       Fix as root and then retry without -r:
         sudo usermod --add-subuids 100000-165535 --add-subgids 100000-165535 $USER
         podman system migrate
         podman unshare cat /proc/self/uid_map   # must show at least 2 lines
       Or re-run this script with -r to use the root podman storage."
    fi
fi

# ---------------------------------------------------------------- 2. image
has_image() { "${engine[@]}" image inspect "$IMAGE" > /dev/null 2>&1; }

case $pull in
    always)
        echo "pulling $IMAGE ..."
        "${engine[@]}" pull --platform "$PLATFORM" "$IMAGE" || die "pull failed"
        ;;
    auto)
        if ! has_image; then
            echo "pulling $IMAGE ..."
            "${engine[@]}" pull --platform "$PLATFORM" "$IMAGE" || die "pull failed"
        fi
        ;;
    never)
        has_image || die "$IMAGE is not in the local store (drop -n or pull it first)"
        ;;
esac

# ------------------------------------------------------- 3. export gcc_local
tmp=
cid=
partial=
cleanup() {
    if [[ -n $partial ]]; then rm -f "$partial"; fi
    if [[ -n $tmp ]]; then rm -rf "$tmp"; fi
    if [[ -n $cid ]]; then "${engine[@]}" rm -f "$cid" > /dev/null 2>&1 || true; fi
}
trap cleanup EXIT

tmp=$(mktemp -d "${TMPDIR:-/tmp}/gcc-local.XXXXXX") || die "mktemp"
echo "creating a container from $IMAGE ..."
cid=$("${engine[@]}" create --platform "$PLATFORM" "$IMAGE") || die "create failed"

echo "exporting the image (~700 MB uncompressed, only root/gcc_local is kept) ..."
"${engine[@]}" export "$cid" | tar -x --no-same-owner -C "$tmp" root/gcc_local \
    || die "export failed — does $IMAGE contain /root/gcc_local?"

[[ -x $tmp/root/gcc_local/bin/$cc ]] \
    || die "unexpected image contents: root/gcc_local/bin/$cc is missing"

# ---------------------------------------------------------------- 4. repack
mkdir -p "$(dirname "$OUT")"
partial=$OUT.part.$$
tar -czf "$partial" --owner=0 --group=0 --numeric-owner -C "$tmp/root" gcc_local \
    || die "tar failed"
mv -f "$partial" "$OUT"
partial=

# Both reference archives contain exactly this path; a quick sanity check that
# we packed the right directory level.  (No grep -q: it would close the pipe
# early and pipefail would report tar's SIGPIPE as an error.)
tar -tzf "$OUT" | grep -x "gcc_local/bin/$cc" > /dev/null \
    || die "archive sanity check failed"

echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
