#!/bin/bash
# guest-tcc-repro.sh — MEASURES which of the programs in tcc/toaru/repro/ kills the guest,
# and on death captures the QEMU state while the guest is still at hand.
#
#   ./guest-tcc-repro.sh                 all programs in repro/
#   ./guest-tcc-repro.sh ctor_argc ...   only the named ones (without the .c suffix); they are
#                                        looked up in repro/ and then in tcc/toaru/ (test.c)
#   ARGS="a b" ./guest-tcc-repro.sh ...  program arguments in the guest
#   REPEAT: run the script several times with your own OUT, e.g.
#       for i in 1 2 3; do OUT=_build/repro/r$i ./guest-tcc-repro.sh ctor_argc; done
#   PORT=8012 TIMEOUT=40 BOOTS=3 BOOTWAIT=60 ./guest-tcc-repro.sh
#       TIMEOUT ... how long to wait for the program to finish (a fatal one never does)
#       BOOTWAIT .. how long to wait for the guest to respond (with KVM ~8 s, longer without)
#
# Procedure: every program gets its OWN BOOT. After a fatal program the guest no longer
# responds (no character on the line, getty does not accept), so there is nothing left
# to continue with - that is why this cannot be one boot for everything. Before each
# boot QEMU is stopped and the log is deleted, so the outputs do not mix.
#
# Results (always for one program):
#   _build/repro/<p>.log        what the guest printed to serial (ttyS1)
#   _build/repro/<p>.ttyS0.log  what the kernel printed (ttyS0 — panic? nothing?)
#   _build/repro/<p>.out        program output between the BEG/END markers
#   _build/repro/<p>.rc         return code as seen by esh in $? — CAREFUL, esh does
#                               NOT have the return value of an external program in
#                               $? (measured 2026-11-12 with a program that returned
#                               3: esh printed 0, see tcc/toaru/repro/exit3.c). The
#                               value here therefore behaves as "the shell finished
#                               the program", not as a status.
#   _build/repro/<p>.dead.*     on death: info cpus, info registers, disas,
#                               and %CPU of the QEMU threads (100 % = something is spinning)
#
# Return code: 0 = no program killed the boot, 1 = some did.
set -uo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=${OUT:-${DIR}/_build/repro}
BIN=${BIN:-${OUT}/bin}
TCC=${TCC:-${DIR}/_build/tcc-toaru/bin/x86_64-toaru-tcc}
LIB=${LIB:-${DIR}/_build/tcc-toaru/lib}
SRC=${SRC:-${DIR}/tcc/toaru/repro}
PORT=${PORT:-8013}
TIMEOUT=${TIMEOUT:-40}          # how many seconds we wait for the program's end marker
BOOTS=${BOOTS:-3}               # how many times to retry the boot when ToaruOS does not come up
VM=${VM:-${DIR}/toaru-vm.sh}
WORK=${WORK:-${DIR}/_build/work}
ARGS=${ARGS:-}                # arguments the program receives in the guest
RUN=$(date +%s)

src_of() {  # program source: repro/ first, then tcc/toaru/ (test.c, hello.c)
    local p=$1
    [[ -f $SRC/$p.c ]] && { echo "$SRC/$p.c"; return 0; }
    [[ -f $DIR/tcc/toaru/$p.c ]] && { echo "$DIR/tcc/toaru/$p.c"; return 0; }
    return 1
}

thread_burn() { # thread_burn <QEMU pid> - how much CPU each thread burns over
                # 2 s. "ps -o pcpu" is enough for that: it is the average since
                # process start, whereas here we care about the moment after guest death.
    python3 -I - "$1" <<'PY'
import os, sys, time
pid = sys.argv[1]
base = '/proc/%s/task' % pid

def snap():
    d = {}
    try:
        tids = os.listdir(base)
    except OSError:
        return d
    for tid in tids:
        try:
            txt = open('%s/%s/stat' % (base, tid)).read()
        except OSError:
            continue
        comm = txt[txt.find('(') + 1:txt.rfind(')')]
        # field 2 (comm) can contain spaces and parentheses, so it is cut from
        # the last parenthesis; then index 11 = utime, 12 = stime
        f = txt[txt.rfind(')') + 2:].split()
        try:
            d[tid] = (int(f[11]) + int(f[12]), comm)
        except (IndexError, ValueError):
            pass
    return d

a, b = snap(), None
time.sleep(2.0)
b = snap()
hz = os.sysconf('SC_CLK_TCK')
print('# QEMU threads %s: CPU usage over 2 s (ticks %d Hz)' % (pid, hz))
for tid in sorted(b, key=lambda t: -(b[t][0] - a.get(t, (0, ''))[0])):
    d = b[tid][0] - a.get(tid, (0, b[tid][1]))[0]
    print('  %-8s %-18s %5.2f cores' % (tid, b[tid][1], d / 2.0 / hz))
PY
}

die() { echo "error: $*" >&2; exit 2; }
[[ -x $TCC ]] || die "missing $TCC — ./mktcc.sh"
[[ -f $LIB/libc.a ]] || die "missing sysroot $LIB — ./mktcc.sh"
[[ -f $WORK/../share.img ]] || die "missing _build/share.img — ./mkshare.sh"

if [[ $# -gt 0 ]]; then
    PROGS=("$@")
else
    PROGS=()
    for f in "$SRC"/*.c; do PROGS+=("$(basename "$f" .c)"); done
fi
(( ${#PROGS[@]} )) || die "no sources in $SRC"

mkdir -p "$OUT" "$BIN"
HTTP=""
cleanup() { [[ -n $HTTP ]] && kill "$HTTP" 2> /dev/null; }
trap cleanup EXIT
trap 'cleanup; die "interrupted"' INT TERM

# ------------------------------------------------------------------ helpers
# alive [s] - does the shell respond?
#  * requires the WHOLE LINE == MARK, because getty also echoes what we typed
#    (the line "echo MARK" would suggest the shell is alive even if it ran nothing)
#  * the command is SENT AGAIN AND AGAIN: the first attempt arrives before getty reads
#    anything (measured 2026-11-12: first reply 8 s after start). With a single send and
#    12 s of waiting every boot looks like a failure - that was the first second attempt
#    of this script, which resulted in "boot failed" for all programs.
alive() {
    local max=${1:-15} mark="PING-$RUN-$RANDOM" i
    for i in $(seq 1 "$max"); do
        "$VM" send "echo $mark" > /dev/null 2>&1 || return 1
        "$VM" log 100000 | awk -v m="$mark" '$0 == m {f = 1} END {exit !f}' && return 0
        sleep 1
    done
    return 1
}

boot() {    # the VM's own boot; retried BOOTS times when ToaruOS occasionally hangs at startup
    local n
    for n in $(seq 1 "$BOOTS"); do
        "$VM" stop > /dev/null 2>&1
        sleep 1
        "$VM" start > /dev/null || return 1
        if alive "${BOOTWAIT:-60}"; then return 0; fi
        echo "    (boot $n not responding within ${BOOTWAIT:-60} s, retrying)"
    done
    return 1
}

# ------------------------------------------------------------- compiling programs
declare -A STATUS
for p in "${PROGS[@]}"; do
    src=$(src_of "$p") || die "missing source $SRC/$p.c or $DIR/tcc/toaru/$p.c"
    # "NULL redefined" from the ToaruOS headers is expected (README 5h)
    "$TCC" -B"$LIB" -o "$BIN/$p.bin" "$src" > "$OUT/$p.compile.log" 2>&1 \
        || die "compile $p: see $OUT/$p.compile.log"
    echo "compiled $p.bin ($(wc -c < "$BIN/$p.bin") B)"
done

# ------------------------------------------------------------- fetch server
# the guest is reached via fetch, not via the shared disk: "cp /mnt/share/..." in
# ToaruOS fails silently ("Bad address") and leaves the files missing (README §5i)
cd "$BIN" || die "missing $BIN"
python3 -I -m http.server "$PORT" --bind 127.0.0.1 > /dev/null 2>&1 &
HTTP=$!
cd "$DIR" || die
sleep 1

# ------------------------------------------------------------------- measurement
DEAD=0
for p in "${PROGS[@]}"; do
    echo
    echo "== $p =========================================="
    boot || { echo "  BOOT FAILED - cannot measure"; STATUS[$p]=boot-failed; DEAD=1; continue; }
    "$VM" send "fetch http://10.0.2.2:$PORT/$p.bin -o /tmp/$p.bin; chmod 755 /tmp/$p.bin; echo ==BEG-$p-$RUN==; /tmp/$p.bin $ARGS; echo ==RC-$p-$RUN== \$?; echo ==END-$p-$RUN==" > /dev/null

    end=0
    for i in $(seq 1 "$TIMEOUT"); do
        "$VM" log 100000 | awk -v m="==END-$p-$RUN==" '$0 == m {f = 1} END {exit !f}' && { end=1; break; }
        sleep 1
    done

    "$VM" log 100000 > "$OUT/$p.log" 2> /dev/null
    cp "$WORK/ttyS0.log" "$OUT/$p.ttyS0.log" 2> /dev/null
    awk -v b="==BEG-$p-$RUN==" -v e="==END-$p-$RUN==" \
        '$0 == b {f = 1; next} $0 == e {f = 0} f && $0 !~ /^root@/ && $0 !~ /^==RC-/' \
        "$OUT/$p.log" > "$OUT/$p.out" 2> /dev/null

    # return code as seen by esh: the line "==RC-<p>-<RUN>== <number>". getty
    # also echoes what we typed — those lines start with "root@" and are
    # wrapped to terminal width, so we require the whole line and a digit after the marker.
    rc=$(awk -v m="==RC-$p-$RUN==" '$1 == m && $2 ~ /^[0-9]+$/ {v = $2} END {print v}' \
            "$OUT/$p.log" 2> /dev/null)
    printf '%s\n' "${rc:-unknown}" > "$OUT/$p.rc"

    if (( end )); then
        echo "  RAN (output $([ -s "$OUT/$p.out" ] && wc -l < "$OUT/$p.out" || echo 0) lines, RC=${rc:-unknown})"
        sed 's/^/    | /' "$OUT/$p.out"
        STATUS[$p]=ok
    else
        echo "  NOT FINISHED within $TIMEOUT s — checking whether the guest is still alive..."
        if alive; then
            echo "  PROGRAM FAILED, guest is alive (it is not the fatal error)"
            STATUS[$p]=failed
        else
            echo "  *** FATAL: guest does not respond, QEMU still running. QEMU state:"
            STATUS[$p]=fatal; DEAD=1
            # 1) how much CPU the QEMU threads are currently spinning (100 % = there is a loop somewhere,
            #    ~0 % = everyone is waiting and nobody progresses) — must be measured
            #    BEFORE the stop, after the stop QEMU always spins zero
            thread_burn "$(cat "$WORK/qemu.pid")" > "$OUT/$p.dead.threads" 2>&1
            # 2) only then are the vCPUs frozen and the registers can be read
            "$VM" mon 'stop' > "$OUT/$p.dead.stop" 2>&1
            { echo "== info cpus =="; "$VM" mon 'info cpus'; } > "$OUT/$p.dead.cpus" 2>&1
            { echo "== info registers -a =="; "$VM" mon 'info registers -a'; } > "$OUT/$p.dead.regs" 2>&1
            { echo "== x/16i \$pc =="; "$VM" mon 'x/16i $pc'; } > "$OUT/$p.dead.dis" 2>&1
            { echo "== info pic =="; "$VM" mon 'info pic'; } > "$OUT/$p.dead.pic" 2>&1
            sed 's/^/    | /' "$OUT/$p.dead.threads" "$OUT/$p.dead.dis" 2> /dev/null
            grep -aE 'RIP|CR0|CR4' "$OUT/$p.dead.regs" 2> /dev/null | sed 's/^/    /' | head -6
            echo "    details: $OUT/$p.dead.{threads,cpus,regs,dis,pic}"
            echo "    kernel (ttyS0):"; sed 's/^/    | /' "$OUT/$p.ttyS0.log" 2> /dev/null | tail -8
        fi
    fi
done

"$VM" stop > /dev/null 2>&1
cleanup; HTTP=""

echo
echo "== summary =========================================="
for p in "${PROGS[@]}"; do
    printf '  %-20s %-12s RC=%s\n' "$p" "${STATUS[$p]:-?}" \
        "$(cat "$OUT/$p.rc" 2> /dev/null || echo '?')"
done
echo "results: $OUT"
(( DEAD == 0 )) && echo "repro: no program killed the boot" || echo "repro: SOME PROGRAM KILLED THE BOOT"
exit $DEAD
