#!/usr/bin/env python3
"""Runs one live-patch case: starts the test program under the host, replaces the
watched source with its v2 version once the program runs, and requires that the
program exits 0 (patched, state intact) and that the host reports the patch.

The watched source is a copy in the build tree; it's restored to v1 afterwards
(content and timestamp), so the next build doesn't recompile the test program.
"""
import argparse
import os
import queue
import shutil
import signal
import subprocess
import sys
import threading
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--source", required=True, help="the copy the program was built from")
    ap.add_argument("--v1", required=True)
    ap.add_argument("--v2", required=True)
    ap.add_argument("--timeout", type=float, default=45)
    args = ap.parse_args()

    stat = os.stat(args.source)
    with open(args.v1, "rb") as f:
        v1 = f.read()
    with open(args.source, "rb") as f:
        if f.read() != v1:
            print(f"error: {args.source} does not match {args.v1}; rebuild first")
            return 1

    proc = subprocess.Popen([args.host, "--", args.exe], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1,
                            start_new_session=True)
    lines: "queue.Queue[str | None]" = queue.Queue()

    def pump() -> None:
        assert proc.stdout
        for line in proc.stdout:
            lines.put(line)
        lines.put(None)

    threading.Thread(target=pump, daemon=True).start()
    output = []
    swapped = False
    deadline = time.monotonic() + args.timeout
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                print("error: timed out")
                os.killpg(proc.pid, signal.SIGKILL)
                break
            try:
                line = lines.get(timeout=remaining)
            except queue.Empty:
                continue
            if line is None:
                break
            output.append(line)
            sys.stdout.write(line)
            # Swap once the host watches the sources and the program is looping.
            if not swapped and line.startswith("frame 2 "):
                shutil.copyfile(args.v2, args.source)
                swapped = True
        code = proc.wait(timeout=10)
    finally:
        shutil.copyfile(args.v1, args.source)
        os.utime(args.source, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)

    text = "".join(output)
    if code != 0:
        print(f"error: host exited with {code}")
        return 1
    if "[hotpp] patch 1 applied" not in text:
        print("error: the host did not report patch 1 as applied")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
