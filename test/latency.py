#!/usr/bin/env python3
"""Run a program with its stdin and stdout delayed, like a slow link.

    latency.py RTT_MS PROGRAM [ARGS...]

Every chunk is delivered RTT_MS/2 after it was sent, in order, both ways, so a
request and its reply take RTT_MS. Used by fake-ssh for ISF_TEST_LATENCY_MS."""

import os
import subprocess
import sys
import threading
import time


def relay(src, dst, delay):
    """Copy src to dst, each chunk DELAY seconds after it arrived"""
    queue = []
    cond = threading.Condition()
    done = False

    def reader():
        nonlocal done
        while True:
            data = os.read(src, 65536)
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
        wait = due - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        try:
            os.write(dst, data)
        except OSError:
            break
    os.close(dst)


def main():
    delay = int(sys.argv[1]) / 2000
    child = subprocess.Popen(sys.argv[2:], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    assert child.stdin and child.stdout
    up =threading.Thread(target=relay, args=(0, child.stdin.fileno(), delay))
    up.start()
    relay(child.stdout.fileno(), 1, delay)
    child.wait()


if __name__ == "__main__":
    main()
