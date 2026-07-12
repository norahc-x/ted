#!/usr/bin/env python3
"""drive.py '<keys>' <cmd> [args...]

Runs a TUI program inside a pseudo-terminal (fixed at 30x100 so screen
coordinates are deterministic), feeds it a scripted keystroke string, and
exits with the program's exit code (99 on hang).

Keys use python escapes: '\\x13' is Ctrl-S, '\\x1b[B' is arrow-down,
'\\x1b[<0;6;1M' is a mouse click at column 6, row 1.
"""
import os
import pty
import sys
import time
import select
import fcntl
import termios
import struct
import signal

keys = sys.argv[1].encode("utf-8").decode("unicode_escape").encode("latin1")
cmd = sys.argv[2:]

pid, fd = pty.fork()
if pid == 0:
    os.execvp(cmd[0], cmd)

fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
out = b""


def drain(t=0.05):
    global out
    while select.select([fd], [], [], t)[0]:
        try:
            d = os.read(fd, 65536)
        except OSError:
            return False
        if not d:
            return False
        out += d
    return True


time.sleep(0.5)  # let the editor enter raw mode
drain()
# Chunked writes with drains between: one big write can deadlock the pty
# (program blocked writing frames, driver blocked writing keys).
CHUNK = 256
for k in range(0, len(keys), CHUNK):
    os.write(fd, keys[k:k + CHUNK])
    drain(0.02)

status = None
deadline = time.time() + 15
while time.time() < deadline:
    drain(0.1)
    wpid, st = os.waitpid(pid, os.WNOHANG)
    if wpid == pid:
        status = st
        break

if status is None:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    sys.stdout.buffer.write(out)
    print("\n== TIMEOUT ==", file=sys.stderr)
    sys.exit(99)

drain()
sys.stdout.buffer.write(out)
sys.exit(os.waitstatus_to_exitcode(status))
