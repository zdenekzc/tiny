#!/bin/bash
# guest-tcc.sh — runs TinyCC-compiled programs for ToaruOS in the guest and compares
# their output and return code with same-named programs compiled by the
# host gcc (same source, see tcc/toaru/*.c).
#
#   ./mktcc.sh ; ./toaru-vm.sh start ; ./guest-tcc.sh ; ./toaru-vm.sh stop
#   PROGS="hello" ./guest-tcc.sh
#
# Expects a logged-in getty (toaru-vm.sh start) and a built cross compiler
# _build/tcc-toaru/bin/x86_64-toaru-tcc (mktcc.sh). Images get into the guest via
# python3 -m http.server and fetch — cp from /mnt/share is silently broken half
# the time ("cp: failed to copy ...: Bad address", measured 2026-11-12, README §5i).
set -uo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=${OUT:-${DIR}/_build/guest-tcc}
PORT=${PORT:-8010}
TCC=${TCC:-${DIR}/_build/tcc-toaru/bin/x86_64-toaru-tcc}
LIB=${LIB:-${DIR}/_build/tcc-toaru/lib}
SRC=${SRC:-${DIR}/tcc/toaru}
# "test" is omitted from the default list because in the guest it kills the boot
# (its main reads argc and it has a constructor) - enable it with PROGS="hello test",
# measured 2026-11-12, details tcc/toaru/README.md
PROGS=${PROGS:-hello}
CC=${CC:-gcc}

die() { echo "error: $*" >&2; exit 2; }
[[ -x $TCC ]] || die "missing $TCC — ./mktcc.sh"
[[ -f $LIB/libc.a ]] || die "missing sysroot $LIB — ./mktcc.sh"
[[ $("$DIR/toaru-vm.sh" status) == *"QEMU is running"* ]] || die "VM is not running — ./toaru-vm.sh start"

args_for() {   # arguments the program gets in the guest and on the host
    case $1 in
        test) echo "one two" ;;
        *)    echo "" ;;
    esac
}

rm -rf "$OUT"; mkdir -p "$OUT"
for p in $PROGS; do
    src=$SRC/$p.c
    [[ -f $src ]] || die "missing source $src"
    # the "NULL redefined" warning from the ToaruOS headers is expected (README §5h)
    "$TCC" -B"$LIB" -o "$OUT/$p.bin" "$src" > "$OUT/$p.log" 2>&1 ||
        { echo "FAIL  $p: tcc compile (see $OUT/$p.log)"; continue; }
    $CC -w -o "$OUT/$p.lin" "$src" >> "$OUT/$p.log" 2>&1 || die "missing $CC on the host"
done

python3 -m http.server "$PORT" --bind 127.0.0.1 -d "$OUT" > /dev/null 2>&1 &
HTTP=$!
trap 'kill $HTTP 2>/dev/null' EXIT
sleep 1

wait_for() {   # wait_for <line> - until it appears in the log (at most 60 s)
    local i
    for i in $(seq 1 60); do
        [[ $'\n'$("$DIR/toaru-vm.sh" log 5)$'\n' == *$'\n'"$1"$'\n'* ]] && return 0
        sleep 1
    done
    return 1
}
RUN=$(date +%s)                     # markers unambiguous even with several runs in one log
for p in $PROGS; do
    [[ -f $OUT/$p.bin ]] || continue
    a=$(args_for "$p")
    "$DIR/toaru-vm.sh" send "fetch http://10.0.2.2:$PORT/$p.bin -o /tmp/$p.bin; chmod 755 /tmp/$p.bin; echo ==BEG-$p-$RUN==; /tmp/$p.bin $a; echo ==RC-$p-$RUN== \$?; echo ==END-$p-$RUN=="
    wait_for "==END-$p-$RUN==" || echo "warning: $p did not finish within 60 s"
done
"$DIR/toaru-vm.sh" log 100000 > "$OUT/guest.log"

norm() {   # the program path differs (./test.lin vs /tmp/test.bin), everything else must match
    sed -E 's|^(argv\[0\]=).*$|\1PROG|'
}
fail=0
for p in $PROGS; do
    [[ -f $OUT/$p.bin ]] || { fail=1; continue; }
    a=$(args_for "$p")
    ( cd "$OUT" && "./$p.lin" $a ) > "$OUT/$p.lin.out" 2>&1
    linrc=$?
    # the ==RC-...== line sits between BEG and END, so it must not land in the
    # block that gets compared with the gcc output
    awk -v b="==BEG-$p-$RUN==" -v e="==END-$p-$RUN==" \
        '$0==b{f=1;next} $0==e{f=0} f && !/^root@/ && !/^==RC-/' \
        "$OUT/guest.log" | norm > "$OUT/$p.toaru.out"
    awk -v m="==RC-$p-$RUN== " 'index($0,m)==1 { sub(m, ""); print $1; exit }' \
        "$OUT/guest.log" > "$OUT/$p.toaru.rc"
    norm < "$OUT/$p.lin.out" > "$OUT/$p.lin.norm"
    torc=$(cat "$OUT/$p.toaru.rc")
    # The guest RC is not what it looks like: esh has no exit status of external
    # programs in $? (measured 2026-11-12 - a program returning 3 showed $? 0,
    # see tcc/toaru/repro/exit3.c), so this comparison only proves "the guest
    # returned 0"; the output comparison above is what actually tests tcc.
    if [[ -s $OUT/$p.toaru.out ]] && cmp -s "$OUT/$p.lin.norm" "$OUT/$p.toaru.out" &&
       [[ ${torc:-x} == "$linrc" ]]; then
        echo "ok    $p - output matches $CC, guest returned $linrc (\$? cannot be measured in esh) — $(wc -l < "$OUT/$p.toaru.out") lines"
    else
        echo "FAIL  $p - rc host=$torc vs $CC=$linrc"; diff "$OUT/$p.lin.norm" "$OUT/$p.toaru.out" | head -8
        fail=1
    fi
done
[[ $fail == 0 ]] && echo "guest-tcc: OK" || echo "guest-tcc: SOME FAILED"
exit $fail
