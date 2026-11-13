#!/bin/bash
# toaru-vm.sh — ToaruOS in QEMU without podman/docker, controlled over the serial line.
#
#   ./toaru-vm.sh start            # starts the VM in the background, log is _build/work/out.log
#   ./toaru-vm.sh send 'ls /'      # sends a command to the guest (assumes a logged-in getty)
#   ./toaru-vm.sh log [N]          # prints N lines of the log without control characters
#   ./toaru-vm.sh status           # running? + log size
#   ./toaru-vm.sh stop             # kills QEMU and the FIFO holder
#   ./toaru-vm.sh mon 'info reg'   # QEMU monitor (HMP) command via the QMP socket
#   ./toaru-vm.sh qmp '{"execute":"query-status"}'
#                                  # raw QMP command (JSON), e.g. input-send-event
#
# The QMP socket (mon) serves to inspect the state when the guest "does not
# respond": until you look at where the vCPU is standing, you cannot tell
# whether the program is stuck in an infinite loop in user space, or the
# kernel crashed (see the boot notes in tcc/toaru/README.md).
# -nographic muxes the monitor onto stdio, which the FIFO holds, hence a
# separate QMP socket.
#
# Way out of the machine: only the serial line (log) or the network. The shared
# disk is one-way — ata.ko does not flush the write-back cache (reason in README.md).
#
# Optional: ISO=/path SHARE=/path RAM= MB SMP= SOUND=none|alsa|off \
#            VISIBLE=1 (QEMU window instead of -display none) \
#            HEADLESS=0 (graphical boot; no getty on ttyS1 — use the Terminal) \
#            ACCEL=tcg QEMU_EXTRA='-d int -D /tmp/qemu-int.log' (additional QEMU
#            arguments; -d int logs exceptions only in TCG, not with KVM)
#   ./toaru-vm.sh start
set -u

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ISO=${ISO:-${DIR}/_build/download/toaruos-2.3.2.iso}
SHARE=${SHARE:-${DIR}/_build/share.img}
WORK=${WORK:-${DIR}/_build/work}
RAM=${RAM:-1024}
SMP=${SMP:-2}
VISIBLE=${VISIBLE:-0}
HEADLESS=${HEADLESS:-1}
DISPLAY_BACKEND=${DISPLAY_BACKEND:-gtk}
FIFO=${WORK}/in
OUT=${WORK}/out.log
# ttyS0 is where the ToaruOS kernel writes its own messages, including panics.
# It used to be "-serial null" and every crash was thrown away, which made a
# dead guest indistinguishable from a slow one.
SER0=${WORK}/ttyS0.log
PIDF=${WORK}/qemu.pid
HOLDERF=${WORK}/holder.pid
QMPS=${WORK}/qmp.sock

die() { echo "error: $*" >&2; exit 1; }

# A PID can be RECYCLED and assigned to another process — a transferred *.pid
# file is exactly such a case (a PID from another boot/machine). That is why we
# also remember the cmdline for each PID and kill only the process we started.
snapshot() { tr '\0' ' ' < "/proc/$1/cmdline" > "$2" 2>/dev/null || : > "$2"; }

mine() {
    local f=$1 want
    [[ -f $f ]] || return 1
    local p; p=$(cat "$f")
    [[ -n "$p" ]] || return 1
    [[ -r /proc/$p/cmdline ]] || return 1
    want=$(cat "${f%.pid}.cmd" 2>/dev/null)
    if [[ -z $want ]]; then
        # old state without *.cmd — at least check that it is our QEMU
        grep -qa qemu "/proc/$p/cmdline" 2>/dev/null && return 0 || {
            echo "warning: pid $p from $f has no saved cmdline and is not qemu — not killing anything" >&2; return 1; }
    fi
    [[ "$(tr '\0' ' ' < "/proc/$p/cmdline")" == "$want"* ]]
}

running() { mine "${PIDF}"; }

do_start() {
    [[ -f "$ISO" ]] || die "missing ISO: $ISO (rename it or use ISO=/path)"
    [[ -f "$SHARE" ]] || die "missing disk image: $SHARE"
    command -v qemu-system-x86_64 >/dev/null || die "missing qemu-system-x86_64"
    # the path to a unix socket fits in ~108 characters (sockaddr_un structure),
    # too deep a WORK would only show up as "QEMU failed to start"
    (( ${#QMPS} < 100 )) || die "path to the QMP socket is too long: $QMPS"
    running && die "QEMU is already running (pid $(cat "${PIDF}")) — ./toaru-vm.sh stop"

    mkdir -p "$WORK"
    rm -f "$FIFO" "$QMPS"; : > "$OUT"; : > "$SER0"; mkfifo "$FIFO"

    # Keep the FIFO open for writing so the guest does not wait for EOF on every command.
    # Stderr/stdin must be redirected away: when someone calls "run-toaru.sh | something",
    # the holder would inherit the write end of that pipe and the waiting reader (tail)
    # would never see EOF — the script would "hang" for two hours even after the work is done.
    sleep 7200 > "$FIFO" 2>/dev/null < /dev/null &
    echo $! > "${HOLDERF}"

    # ACCEL=tcg is useful for debugging: QEMU -d int (exception log) works only with TCG.
    local -a accel=(-accel kvm)
    if [[ ${ACCEL:-kvm} == tcg ]]; then
        accel=(-accel tcg)
    elif [[ -r /dev/kvm && -w /dev/kvm ]]; then
        :
    else
        echo "warning: no /dev/kvm — TCG, slow"; accel=()
    fi

    # SOUND=none (default) wires AC97 to QEMU's "none" backend.  Plain
    # "-device AC97" makes QEMU open the host's default backend (ALSA) and on a
    # machine without a live sound server the vCPUs then never get a single tick
    # - the guest produces no output at all and never reaches a prompt
    # (measured 2026-11-12: 0 vCPU jiffies in /proc/<qemu pid>/task/.../stat,
    # the same command line without AC97 boots in ~4 s).  The device itself
    # stays, ToaruOS enumerates PCI at boot; only the backend is inert.
    local -a sound=()
    case ${SOUND:-none} in
        none) sound=(-audiodev none,id=snd0 -device AC97,audiodev=snd0) ;;
        alsa) sound=(-device AC97) ;;
        off)  ;;
        *)    die "SOUND should be none|alsa|off" ;;
    esac

    # VISIBLE=1 opens a QEMU window (gtk/sdl) instead of -display none.  HEADLESS=0
    # lets ToaruOS boot graphically (yutani + terminal).  In that mode
    # 99_runstart.sh execs the compositor instead of getty, so ttyS1 stays
    # silent; commands must be typed into the graphical Terminal (run-toaru.sh
    # does that with the QEMU monitor's sendkey).
    local -a display=(-display none) bootmode=()
    [[ $VISIBLE == 1 ]] && display=(-display "$DISPLAY_BACKEND")
    [[ $HEADLESS == 1 ]] && bootmode=(-fw_cfg name=opt/org.toaruos.bootmode,string=headless)
    case $HEADLESS in 0|1) ;; *) die "HEADLESS should be 0 or 1" ;; esac
    case $VISIBLE in 0|1) ;; *) die "VISIBLE should be 0 or 1" ;; esac

    # -M pc is intentional: ata.ko binds to the PIIX IDE 8086:7010/8086:7111 and on q35
    # ToaruOS cannot even read its own CD. Network: the only NIC in ToaruOS is e1000.
    # fw_cfg arranges booting without graphics and getty on the second serial line.
    #
    # -display none, not -nographic: -nographic muxes the monitor into stdio, which
    # is where the FIFO is held here, and the human-monitor-command then returns only
    # GenericError via QMP (HMP on the shared line has nowhere to send the result). Without
    # -nographic the QMP socket is separate and the guest is the same: ttyS1 still
    # goes to stdio via "-serial mon:stdio", ttyS0 to a file.
    qemu-system-x86_64 "${accel[@]}" \
        -M pc -m "$RAM" -smp "$SMP" \
        -cdrom "$ISO" -vga std "${sound[@]}" \
        -drive format=raw,if=none,id=share,file="$SHARE" \
        -device ide-hd,drive=share,bus=ide.0,unit=0 \
        -netdev user,id=n0 -device e1000,netdev=n0 \
        "${bootmode[@]}" \
        -fw_cfg name=opt/org.toaruos.gettyargs,string='-a root /dev/ttyS1 115200 linux' \
        -qmp "unix:$QMPS,server,nowait" \
        -no-reboot "${display[@]}" -serial "file:$SER0" -serial mon:stdio \
        ${QEMU_EXTRA:-} < "$FIFO" > "$OUT" 2>&1 &
    echo $! > "${PIDF}"
    # cmdline only after exec qemu (right after & it may still be a bash subprocess;
    # stop would then not recognize QEMU as our own and leave it running)
    local q k
    q=$(cat "${PIDF}")
    for k in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        grep -qa '^qemu-system' "/proc/$q/cmdline" 2>/dev/null && break
        sleep 0.1
    done
    snapshot "$q" "${PIDF%.pid}.cmd"
    # cmdline of the FIFO holder only now: opening the FIFO for writing waits for QEMU (reader),
    # until then it is a bash subprocess with a different cmdline and stop would refuse to kill it
    local h
    h=$(cat "${HOLDERF}")
    for k in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        grep -qa '^sleep' "/proc/$h/cmdline" 2>/dev/null && break
        sleep 0.1
    done
    snapshot "$h" "${HOLDERF%.pid}.cmd"
    disown
    echo "QEMU pid=$(cat "${PIDF}") log=${OUT}"
    echo "boot takes ~15 s; then: ./toaru-vm.sh send 'ls /dev'"
}

do_send() {
    [[ $# -gt 0 ]] || die "send: command, e.g. ./toaru-vm.sh send 'ls /'"
    [[ -p "$FIFO" ]] || die "no FIFO — ./toaru-vm.sh start"
    running || die "QEMU is not running — ./toaru-vm.sh start"
    printf '%s\n' "$*" > "$FIFO"
}

# QMP is JSON over a unix socket; in addition, "human-monitor-command" lets through
# any HMP command (info registers, xp, pmemsave, stop, system_reset...).
# The reply may be preceded by an asynchronous event, so it loops until the
# return value or an error arrives. The argument name is "command-line" - "cmdline"
# is rejected by QEMU with a GenericError that says nothing (measured 2026-11-12).
do_mon() {
    [[ $# -gt 0 ]] || die "mon: HMP command, e.g. ./toaru-vm.sh mon 'info registers -a'"
    [[ -S "$QMPS" ]] || die "no QMP socket $QMPS — ./toaru-vm.sh start"
    python3 -I - "$QMPS" "$*" <<'PY'
import json, socket, sys

path, cmdline = sys.argv[1], " ".join(sys.argv[2:])
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(20.0)
try:
    s.connect(path)
except OSError as e:
    sys.exit("connection to %s: %s" % (path, e))

f = s.makefile("rwb")

def recv_reply():
    # Waits for a message with "return"/"error". The greeting ("QMP") and asynchronous
    # events in between mean nothing, they are just read -- so it does not matter that
    # QEMU 10.0 sends the greeting only after the client's first command
    # (measured 2026-11-12: without sending a command, receiving the greeting would block).
    while True:
        try:
            line = f.readline()
        except (OSError, TimeoutError):
            sys.exit("QMP: reply did not arrive within %.0f s — is QEMU running?" % s.gettimeout())
        if not line:
            sys.exit("QMP: connection closed")
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if "return" in msg or "error" in msg:
            return msg

def call(cmd, args=None):
    msg = {"execute": cmd}
    if args:
        msg["arguments"] = args
    f.write((json.dumps(msg) + "\n").encode()); f.flush()
    return recv_reply()

r = call("qmp_capabilities")
if "error" in r:
    sys.exit("qmp_capabilities: %s" % r["error"])
r = call("human-monitor-command", {"command-line": cmdline})
if "error" in r:
    e = r["error"]
    sys.exit("error %s: %s" % (e.get("class"), e.get("desc", "")))
sys.stdout.write(r.get("return", ""))
PY
}

# Raw QMP: each argument is one JSON message ({"execute": ..., "arguments": ...}).
# Useful for input-send-event, which drives the graphical session independently
# of the host window focus (sendkey only injects keystrokes into the guest's
# keyboard; pointer movement has no HMP equivalent that works with vmmouse).
do_qmp() {
    [[ $# -gt 0 ]] || die "qmp: JSON command, e.g. ./toaru-vm.sh qmp '{\"execute\":\"query-status\"}'"
    [[ -S "$QMPS" ]] || die "no QMP socket $QMPS — ./toaru-vm.sh start"
    python3 -I - "$QMPS" "$@" <<'PY'
import json, socket, sys

path, messages = sys.argv[1], sys.argv[2:]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(20.0)
try:
    s.connect(path)
except OSError as e:
    sys.exit("connection to %s: %s" % (path, e))

f = s.makefile("rwb")

def recv_reply():
    while True:
        try:
            line = f.readline()
        except (OSError, TimeoutError):
            sys.exit("QMP: reply did not arrive within %.0f s" % s.gettimeout())
        if not line:
            sys.exit("QMP: connection closed")
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if "return" in msg or "error" in msg:
            return msg

def call(cmd, args=None):
    msg = {"execute": cmd}
    if args is not None:
        msg["arguments"] = args
    f.write((json.dumps(msg) + "\n").encode()); f.flush()
    return recv_reply()

r = call("qmp_capabilities")
if "error" in r:
    sys.exit("qmp_capabilities: %s" % r["error"])

for raw in messages:
    try:
        msg = json.loads(raw)
    except ValueError as e:
        sys.exit("qmp: invalid JSON (%s): %s" % (e, raw))
    if "execute" not in msg:
        sys.exit("qmp: missing \"execute\" in %s" % raw)
    r = call(msg["execute"], msg.get("arguments"))
    print(json.dumps(r))
    if "error" in r:
        sys.exit(1)
PY
}

clean() {
    # -a: mc and other curses programs send non-printable bytes into the log too
    # (mouse, SGR); without -a grep refuses with "binary file matches" and the log disappears
    sed -e 's/\x1b\[[0-9;?]*[a-zA-Z]//g' -e 's/\x1b[()][A-Z0-9]//g' "$OUT" \
        | tr '\r' '\n' | grep -av '^ *$'
}

case "${1:-}" in
    start)  do_start ;;
    send)   shift; do_send "$*" ;;
    log)    clean | tail -n "${2:-40}" ;;
    mon)    shift; do_mon "$*" ;;
    qmp)    shift; do_qmp "$@" ;;
    status)
        if running; then echo "QEMU is running, pid=$(cat "${PIDF}")";
        else echo "QEMU is not running"; fi
        [[ -f "$OUT" ]] && echo "log: $(wc -c < "$OUT") B, $(wc -l < "$OUT") lines" ;;
    stop)
        for f in "${PIDF}" "${HOLDERF}"; do
            [[ -f "$f" ]] || continue
            p=$(cat "$f" 2>/dev/null)
            if mine "$f"; then
                kill "$p" 2>/dev/null && echo "killed $p ($(cat "${f%.pid}.cmd" 2>/dev/null))"
                # QEMU sometimes ignores SIGTERM (measured 2026-11-11) => SIGKILL after 3 s
                for k in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
                    [[ -d /proc/$p ]] || break
                    sleep 0.2
                done
                [[ -d /proc/$p ]] && kill -9 "$p" 2>/dev/null && echo "  (SIGKILL $p)"
            elif [[ -n "$p" && -r /proc/$p/cmdline ]]; then
                echo "pid $p is alive but is not ours — leaving it alone" >&2
            fi
            rm -f "$f" "${f%.pid}.cmd"
        done
        rm -f "$QMPS" ;;
    *)      awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0" ;;
esac
