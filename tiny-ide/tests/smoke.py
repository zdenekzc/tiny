#!/usr/bin/env python3
"""Drive tiny-ide through a real pseudo-terminal; only Python stdlib is used."""
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

ANSI = re.compile(
    rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\)|[()][0-~]|.)")


def plain(data):
    """Remove ANSI sequences and cell-editing controls from ncurses output."""
    cleaned = ANSI.sub(b"", data).replace(b"\b", b"").replace(b"\r", b"")
    return cleaned.decode("utf-8", "replace")


class Ide:
    def __init__(self, binary, project, source, columns=100, rows=32):
        env = os.environ.copy()
        env.update({"TERM": "xterm", "LC_ALL": "C.UTF-8", "ESCDELAY": "50"})
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        self.process = subprocess.Popen(
            [str(binary), str(source)], cwd=project, env=env,
            stdin=slave, stdout=slave, stderr=slave, close_fds=True,
            start_new_session=True)
        os.close(slave)
        os.set_blocking(self.master, False)
        self.data = bytearray()

    def drain(self):
        while True:
            ready, _, _ = select.select([self.master], [], [], 0)
            if not ready:
                return
            try:
                chunk = os.read(self.master, 65536)
            except (BlockingIOError, OSError):
                return
            if not chunk:
                return
            self.data.extend(chunk)
            if len(self.data) > 4_000_000:
                del self.data[:-2_000_000]

    def output(self):
        return plain(bytes(self.data))[-18000:]

    def wait_for(self, needle, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.drain()
            if needle in plain(bytes(self.data)):
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.025)
        raise AssertionError(f"did not see {needle!r}; process status={self.process.poll()}\n"
                             f"--- filtered terminal output ---\n{self.output()}")

    def wait_exit(self, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.drain()
            status = self.process.poll()
            if status is not None:
                return status
            time.sleep(0.025)
        raise AssertionError(f"process did not exit\n--- terminal ---\n{self.output()}")

    def send(self, value):
        os.write(self.master, value.encode() if isinstance(value, str) else value)

    def stop(self):
        self.drain()
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGKILL)
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            pass
        os.close(self.master)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: smoke.py /path/to/tiny-ide.bin")
    binary = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parents[1]
    require(os.environ.get("TCC"), "TCC must point at a host TinyCC")

    with tempfile.TemporaryDirectory(prefix="tiny-ide-smoke-") as temporary:
        project = Path(temporary)
        source = project / "hello.c"
        shutil.copyfile(root / "demo" / "hello.c", source)
        ide = Ide(binary, project, source)
        try:
            ide.wait_for("Tiny IDE")
            ide.wait_for("hello.c")
            ide.send("\x1b[1;5F")  # Ctrl+End
            ide.send("\r/* smoke-edit */\r")
            time.sleep(0.2)
            ide.send("\x1b[15~")  # F5: save, compile, run
            ide.wait_for("sum of squares 1..10 = 385", timeout=30)
            ide.wait_for("[exit 0]")
            require("/* smoke-edit */" in source.read_text(),
                    "F5 did not save the edited source")
            ide.send("\x1b[1;5F")
            ide.send("/* discard-me */")
            ide.send("\x1b[21~")  # F10
            ide.wait_for("There are unsaved changes")
            ide.send("\x1bOC")    # Right: Discard all (application keypad)
            time.sleep(0.2)
            ide.send("\r")
            status = ide.wait_exit()
            require(status == 0, f"clean exit returned {status}")
            require("discard-me" not in source.read_text(),
                    "Discard all unexpectedly saved the edit")
        finally:
            ide.stop()

        broken = project / "broken.c"
        broken.write_text("int main(void)\n{\n    int value = 1;\n"
                          "    re turn value;\n}\n")
        ide = Ide(binary, project, broken)
        try:
            ide.wait_for("broken.c")
            ide.send("\x1b[18~")  # F7: build only
            ide.wait_for("error:")
            ide.wait_for("[exit 1]")
            ide.send("\x1b[17~")  # F6: editor -> output
            ide.send("\x1bOH")    # Home: first output line
            ide.send("\x1bOB")
            time.sleep(0.1)
            ide.send("\x1bOB")
            time.sleep(0.1)
            ide.send("\r")        # compiler diagnostic
            ide.wait_for("Jumped to broken.c:4")
            ide.send("\x1b[21~")
            status = ide.wait_exit()
            require(status == 0, f"exit after diagnostic returned {status}")
        finally:
            ide.stop()

    print("smoke: edit + save + F5 build/run + program output: OK")
    print("smoke: unsaved-quit confirmation + discard: OK")
    print("smoke: F7 compiler error + diagnostic jump: OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"smoke: FAILED: {error}", file=sys.stderr)
        sys.exit(1)
