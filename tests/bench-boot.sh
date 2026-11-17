#!/bin/bash
# bench-boot.sh — compare ToaruOS boot time from an ISO with QEMU's direct
# multiboot boot (-kernel / -initrd).
#
#   ./tests/bench-boot.sh                  # 7 runs, _import/download/image.iso
#   N=3 ./tests/bench-boot.sh              # fewer runs
#   ISO=_build/netsurf.iso N=5 ./tests/bench-boot.sh
#   KERNEL=/tmp/k INITRD=/tmp/r ./tests/bench-boot.sh
#
# Both modes use identical QEMU devices and the same kernel command line
# (root=/dev/ram0 start=--headless) plus the fw_cfg getty arguments, so the
# guest behaves the same.  The measured point is the first "root@livecd" on
# ttyS1 — the serial getty being ready.
#
# The kernel and ramdisk are extracted from ISO with isoinfo when KERNEL or
# INITRD is not supplied, so both modes always boot the same pair.
#
# Environment:
#   ISO      source ISO                 (default _import/download/image.iso)
#   KERNEL   kernel image               (default: extracted from ISO)
#   INITRD   compressed ramdisk         (default: extracted from ISO)
#   N        measured runs per mode     (default 7; a warm-up run is not counted)
#   RAM      guest memory in MiB        (default 1024)
#   SMP      guest CPUs                 (default 2)
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd "$HERE/.." && pwd)

N=${N:-7}
RAM=${RAM:-1024}
SMP=${SMP:-2}

die() { echo "error: $*" >&2; exit 1; }
. "$TOP/need.sh"   # need(): dependency check with package-install hints
[[ $N =~ ^[0-9]+$ && $N -ge 1 ]] || die "N must be a positive integer"

need qemu-system-x86_64
need isoinfo
need awk
need sort

if [[ -z ${ISO:-} ]]; then
    for f in "$TOP/_import/download/image.iso" \
             "$TOP/_build/download/toaruos-2.3.2.iso"; do
        [[ -f $f ]] && { ISO=$f; break; }
    done
    ISO=${ISO:-$TOP/_import/download/image.iso}
fi
[[ -f $ISO ]] || die "missing ISO $ISO (run ./tests/run-toaru.sh)"

mkdir -p "$TOP/_tmp"
WORK=$(mktemp -d "$TOP/_tmp/bootbench.XXXXXX")
SER0=$WORK/ttyS0.log
SER1=$WORK/ttyS1.log

KERNEL=${KERNEL:-$WORK/kernel}
INITRD=${INITRD:-$WORK/ramdisk.igz}
if [[ ! -s $KERNEL ]]; then
    isoinfo -R -i "$ISO" -x /kernel > "$KERNEL" 2>/dev/null ||
        die "cannot extract /kernel from $ISO"
    [[ -s $KERNEL ]] || die "extracted kernel is empty"
fi
if [[ ! -s $INITRD ]]; then
    isoinfo -R -i "$ISO" -x /ramdisk.igz > "$INITRD" 2>/dev/null ||
        die "cannot extract /ramdisk.igz from $ISO"
    [[ -s $INITRD ]] || die "extracted ramdisk is empty"
fi

if [[ -r /dev/kvm && -w /dev/kvm ]]; then
    accel=(-accel kvm)
else
    accel=(-accel tcg)
    echo "warning: no /dev/kvm — using TCG, numbers are not comparable" >&2
fi

common=("${accel[@]}" -M pc -m "$RAM" -smp "$SMP" -display none -vga std
        -fw_cfg name=opt/org.toaruos.gettyargs,string='-a root /dev/ttyS1 115200 linux'
        -serial "file:$SER0" -serial "file:$SER1" -no-reboot)

QEMU_PID=
cleanup() {
    if [[ -n ${QEMU_PID:-} ]]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

boot() { # boot iso|kernel -> seconds on stdout
    local mode=$1 t0 t1 pid deadline
    : > "$SER0"; : > "$SER1"
    t0=$(date +%s%N)
    if [[ $mode == iso ]]; then
        qemu-system-x86_64 "${common[@]}" \
            -cdrom "$ISO" \
            -fw_cfg name=opt/org.toaruos.bootmode,string=headless \
            >/dev/null 2>&1 &
    else
        qemu-system-x86_64 "${common[@]}" \
            -kernel "$KERNEL" -initrd "$INITRD" \
            -append "root=/dev/ram0 start=--headless" \
            >/dev/null 2>&1 &
    fi
    pid=$!
    QEMU_PID=$pid
    deadline=$((SECONDS + 90))
    while (( SECONDS < deadline )); do
        grep -aq 'root@livecd' "$SER1" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.01
    done
    t1=$(date +%s%N)
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    QEMU_PID=
    grep -aq 'root@livecd' "$SER1" 2>/dev/null ||
        { echo "FAIL" >&2; return 1; }
    awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f", (b - a) / 1e9 }'
}

echo "== ISO:              $ISO"
echo "== kernel:           $KERNEL"
echo "== initrd:           $INITRD"
echo "== runs: $N, RAM: $RAM MiB, SMP: $SMP, accel: ${accel[1]}"

boot iso >/dev/null || die "ISO warm-up boot failed"
boot kernel >/dev/null || die "kernel+initrd warm-up boot failed"

ti=()
tk=()
for i in $(seq 1 "$N"); do
    ti+=("$(boot iso)")
    tk+=("$(boot kernel)")
    printf 'run %d: iso=%ss kernel+initrd=%ss\n' "$i" "${ti[$((i-1))]}" "${tk[$((i-1))]}"
done

stats() {
    printf '%s\n' "$@" | sort -n | awk '
        { a[NR] = $1; s += $1 }
        END {
            n = NR
            median = (n % 2) ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
            printf "n=%d min=%.3f median=%.3f mean=%.3f max=%.3f", n, a[1], median, s / n, a[n]
        }'
}
echo "ISO (El Torito + bootloader): $(stats "${ti[@]}")"
echo "kernel + initrd (multiboot):  $(stats "${tk[@]}")"
