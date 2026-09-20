#!/usr/bin/env python3
"""Run a program with its stdin and stdout delayed, like a slow link.

    latency.py RTT_MS PROGRAM [ARGS...]

Every chunk is delivered RTT_MS/2 after it was sent, in order, both ways, so a
request and its reply take RTT_MS. Used by fake-ssh for ISF_TEST_LATENCY_MS.

With $ISF_TEST_BANDWIDTH_KBPS, the line also carries only that many KB a
second in each direction.

With $ISF_TEST_SFTP_LOG, it also counts the SFTP requests going by, by type,
and the rounds: how many times a request went out after a reply came back
(requests sent together are one round, one after another, one each). The
counts go to $ISF_TEST_SFTP_LOG.<pid> after each request, as "TYPE=N ...",
with start=<when the relay started, in microseconds>: the relay is killed at
the end, not let finish.

It exits with PROGRAM's exit status."""

import fcntl
import os
import subprocess
import sys
import threading
import time

NAMES = {
    1: "INIT", 3: "OPEN", 4: "CLOSE", 5: "READ", 6: "WRITE", 7: "LSTAT",
    8: "FSTAT", 9: "SETSTAT", 10: "FSETSTAT", 11: "OPENDIR", 12: "READDIR",
    13: "REMOVE", 14: "MKDIR", 15: "RMDIR", 16: "REALPATH", 17: "STAT",
    18: "RENAME", 19: "READLINK", 20: "SYMLINK",
}


class Counter:
    """Counts the requests in the client's stream"""

    def __init__(self, path):
        self.path = f"{path}.{os.getpid()}"
        self.buf = b""
        self.counts = {"rounds": 0, "start": time.time_ns() // 1000}
        self.replied = True  # the next request starts a round

    def reply(self):
        self.replied = True

    def requests(self, data):
        self.buf += data
        seen = False
        while len(self.buf) >= 5:
            n = int.from_bytes(self.buf[:4], "big")
            if len(self.buf) < 4 + n:
                break
            kind = self.buf[4]
            name = NAMES.get(kind, str(kind))
            if kind == 200 and n >= 9:  # EXTENDED: by its name
                ln = int.from_bytes(self.buf[9:13], "big")
                name = self.buf[13:13 + ln].decode(errors="replace").split("@")[0]
            self.counts[name] = self.counts.get(name, 0) + 1
            if self.replied:
                self.counts["rounds"] += 1
                self.replied = False
            self.buf = self.buf[4 + n:]
            seen = True
        if seen:
            with open(self.path, "w") as f:
                f.write(" ".join(f"{k}={v}" for k, v in sorted(self.counts.items())) + "\n")


# One line for everything isf opens: each chunk takes its turn on it, so the
# rate is the whole machine's, not each session's. $ISF_TEST_LINE holds when
# the line is free again, under a lock.
LINE = os.environ.get("ISF_TEST_LINE")
line_lock = threading.Lock()


def reserve(span, due):
    """Take SPAN seconds of the line, not before DUE. Returns when the last
    byte lands."""
    with line_lock:
        if not LINE:
            return due + span
        with open(LINE, "a+") as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            f.seek(0)
            free = float(f.read() or 0)
            end = max(free, due, time.monotonic()) + span
            f.seek(0)
            f.truncate()
            f.write(str(end))
            f.flush()
            fcntl.flock(f, fcntl.LOCK_UN)
        return end


def relay(src, dst, delay, seen=None, close=None, rate=0):
    """Copy src to dst, each chunk DELAY seconds after it arrived, telling
    SEEN about each chunk as it arrives. At the end, dst is closed (with
    CLOSE, if it belongs to a file object). With RATE (bytes a second), the
    chunks also queue behind each other, as they would on a slow line."""
    queue = []
    cond = threading.Condition()
    done = False

    def reader():
        nonlocal done
        while True:
            data = os.read(src, 65536)
            if seen and data:
                seen(data)
            with cond:
                if not data:
                    done = True
                else:
                    queue.append((time.monotonic() + delay, data))
                cond.notify()
            if not data:
                return

    threading.Thread(target=reader, daemon=True).start()
    while True:
        with cond:
            while not queue and not done:
                cond.wait()
            if not queue:
                break
            due, data = queue.pop(0)
        if rate:
            due = reserve(len(data) / rate, due)
        wait = due - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        try:
            os.write(dst, data)
        except OSError:
            break
    try:
        close() if close else os.close(dst)
    except OSError:
        pass


def main():
    delay = int(sys.argv[1] or 0) / 2000
    rate = float(os.environ.get("ISF_TEST_BANDWIDTH_KBPS") or 0) * 1024
    counter = Counter(os.environ["ISF_TEST_SFTP_LOG"]) if os.environ.get("ISF_TEST_SFTP_LOG") else None
    child = subprocess.Popen(sys.argv[2:], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    assert child.stdin and child.stdout
    up = threading.Thread(target=relay,
                          args=(0, child.stdin.fileno(), delay, counter and counter.requests, child.stdin.close, rate))
    up.start()
    relay(child.stdout.fileno(), 1, delay, counter and (lambda _data: counter.reply()), None, rate)
    sys.exit(child.wait())


if __name__ == "__main__":
    main()
