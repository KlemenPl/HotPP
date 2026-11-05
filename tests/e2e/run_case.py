#!/usr/bin/env python3
"""Runs one end-to-end hot-reload case.

A case directory holds main.cpp (never edited), case.json, and versions of the
edited sources: v1/ is what the program is built from, later steps copy files
from other directories over the build-tree copies. Each step waits for the host
to print what the step expects (patch applied, rejected, ...), then lets the
program run a few frames on the result. Finally the stop file tells the program
to check its final state; the case passes if it prints PASS and exits 0.

The build-tree copies are restored to v1 (content and timestamps) afterwards, so
the next build doesn't recompile the test program.
"""
import argparse
import json
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import threading
import time


class Output:
    def __init__(self, proc):
        self.lines = []
        self._queue = queue.Queue()
        self.closed = False
        threading.Thread(target=self._pump, args=(proc,), daemon=True).start()

    def _pump(self, proc):
        for line in proc.stdout:
            self._queue.put(line)
        self._queue.put(None)

    def wait_for(self, pattern, start, deadline, count=1):
        """Index of the line after the count-th match of pattern at or after start."""
        rx = re.compile(pattern)
        seen = 0
        i = start
        while True:
            while i < len(self.lines):
                line = self.lines[i]
                i += 1
                if line.startswith("FAIL"):
                    raise RuntimeError(line.strip())
                if rx.search(line):
                    seen += 1
                    if seen == count:
                        return i
            if self.closed:
                raise RuntimeError(f"program exited while waiting for {pattern!r}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError(f"timed out waiting for {pattern!r}")
            try:
                line = self._queue.get(timeout=min(remaining, 0.5))
            except queue.Empty:
                continue
            if line is None:
                self.closed = True
            else:
                self.lines.append(line)
                sys.stdout.write(line)
                sys.stdout.flush()

    def drain(self, timeout):
        deadline = time.monotonic() + timeout
        while not self.closed and time.monotonic() < deadline:
            try:
                line = self._queue.get(timeout=0.2)
            except queue.Empty:
                continue
            if line is None:
                self.closed = True
            else:
                self.lines.append(line)
                sys.stdout.write(line)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--case", required=True, help="case source directory")
    ap.add_argument("--work", required=True, help="build-tree copies the program was built from")
    ap.add_argument("--step-timeout", type=float, default=40)
    args = ap.parse_args()

    with open(os.path.join(args.case, "case.json")) as f:
        spec = json.load(f)
    settle = spec.get("settle_frames", 5)

    # Remember the v1 state of every work file, and refuse to run on a stale build.
    v1_dir = os.path.join(args.case, "v1")
    originals = {}
    for name in os.listdir(v1_dir):
        work_file = os.path.join(args.work, name)
        with open(os.path.join(v1_dir, name), "rb") as f:
            v1 = f.read()
        with open(work_file, "rb") as f:
            if f.read() != v1:
                print(f"error: {work_file} does not match v1; reconfigure and rebuild first")
                return 1
        originals[work_file] = (v1, os.stat(work_file))

    stop_file = os.path.join(args.work, "stop")
    if os.path.exists(stop_file):
        os.remove(stop_file)
    env = dict(os.environ, HOTPP_E2E_STOP=stop_file)
    proc = subprocess.Popen([args.host, "--", args.exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1, start_new_session=True, env=env)
    out = Output(proc)
    error = None
    try:
        t = lambda: time.monotonic() + args.step_timeout
        pos = out.wait_for(r"^\[hotpp\] tracking ", 0, t())  # dependency scan done
        pos = max(pos, out.wait_for(r"^frame 2$", 0, t()))
        for n, step in enumerate(spec["steps"], 1):
            print(f"--- step {n}: {step.get('name', '')}", flush=True)
            for dst, src in step["copy"].items():
                shutil.copyfile(os.path.join(args.case, src), os.path.join(args.work, dst))
            pos = out.wait_for(step["expect"], pos, t())
            pos = out.wait_for(r"^frame \d+$", pos, t(), count=settle)
        open(stop_file, "w").close()
        out.drain(15)
        code = proc.wait(timeout=15)
        if code != 0:
            error = f"host exited with {code}"
        elif not any(line.startswith("PASS") for line in out.lines):
            error = "the program did not report PASS"
    except RuntimeError as e:
        error = str(e)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
        for path, (content, st) in originals.items():
            with open(path, "wb") as f:
                f.write(content)
            os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
        if os.path.exists(stop_file):
            os.remove(stop_file)

    if error:
        print(f"error: {error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
