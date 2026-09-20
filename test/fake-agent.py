#!/usr/bin/env python3
"""A stand-in for isf's agent on the remote (isf --agent FOLDER...).

It watches nothing. It says one thing, at the one moment in a sync when the
local side reads what the agent says: while it waits for a file it sent to be
put in place. $ISF_TEST_SAY_MOVED (FROM:TO, inside the first folder) is
reported as a rename just before the first of those is answered, which is how
the real agent's report of a rename isf itself made can reach the local side
before the report of what isf put there next.

$ISF_TEST_PROTOCOL and $ISF_TEST_VERSION are what it says it speaks.
"""
import os
import sys
import time

folders = [a for a in sys.argv[1:] if a != "--agent"]
out = sys.stdout.buffer


def send(type_, root, value, text=b""):
    out.write(bytes([ord(type_), root]) + b"%016x" % value + text + b"\0")
    out.flush()


def lstat_text(path):
    try:
        st = os.lstat(path)
    except OSError:
        return b"%016x%08x%08x" % (0, 0, 0)
    return b"%016x%08x%08x" % (st.st_size, int(st.st_mtime), st.st_mode)


def place(root, expect, tmp_rel, target_rel):
    """What the agent does for a 'P': lstat and rename, both here"""
    tmp = os.path.join(folders[root], tmp_rel)
    target = os.path.join(folders[root], target_rel)
    if lstat_text(target) != expect:
        os.unlink(tmp)
        return b"c"
    try:
        os.rename(tmp, target)
    except OSError:
        return b"e"  # the local side does it over SFTP instead
    return b"o"


for folder in folders:
    os.makedirs(folder, exist_ok=True)
send("T", 0, int(time.time()))
send("R", 0, int(os.environ["ISF_TEST_PROTOCOL"]), os.environ["ISF_TEST_VERSION"].encode())

moved = os.environ.get("ISF_TEST_SAY_MOVED")
buf = b""
while True:
    chunk = os.read(0, 4096)
    if not chunk:
        break  # the local side is gone
    buf += chunk
    while len(buf) >= 3:
        end = buf.find(b"\0", 2)
        if end < 0:
            break
        msg, buf = buf[:end], buf[end + 1:]
        if msg[0:1] != b"P":
            continue
        root, text = msg[1], msg[2:]
        request = int(text[:16], 16)
        expect = text[16:48]
        length = int(text[48:56], 16)
        tmp_rel = text[56:56 + length].decode()
        target_rel = text[56 + length:].decode()
        if moved:
            frm, to = moved.split(":")
            send("M", 0, len(frm),
                 lstat_text(os.path.join(folders[0], to)) + frm.encode() + to.encode())
            moved = None
        send("p", root, request, place(root, expect, tmp_rel, target_rel))
