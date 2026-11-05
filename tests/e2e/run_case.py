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

With "gdb" in case.json, the program runs under gdb with that script instead,
connected to a separate host, and "expect_output" lists lines the script must
print (in order) after the last step.

With "serve": true, the host runs on its own and the program connects to it; a
step with "restart_host": "KILL" or "TERM" stops the host with that signal and
starts a new one before waiting for the step's lines.
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
    """Lines of one or more processes' stdout, interleaved as they arrive."""

    def __init__(self, procs):
        self.lines = []
        self._queue = queue.Queue()
        self._open = 0
        self.closed = False
        for proc in procs:
            self.add(proc)

    def add(self, proc):
        self._open += 1
        self.closed = False
        threading.Thread(target=self._pump, args=(proc,), daemon=True).start()

    def _pump(self, proc):
        for line in proc.stdout:
            self._queue.put(line)
        self._queue.put(None)

    def _take(self, line):
        if line is None:
            self._open -= 1
            self.closed = self._open == 0
        else:
            self.lines.append(line)
            sys.stdout.write(line)
            sys.stdout.flush()

    def wait_for(self, pattern, start, deadline, count=1):
        """Index of the line after the count-th match of pattern at or after start."""
        if count <= 0:
            return start
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
                self._take(self._queue.get(timeout=min(remaining, 0.5)))
            except queue.Empty:
                continue

    def drain(self, timeout):
        deadline = time.monotonic() + timeout
        while not self.closed and time.monotonic() < deadline:
            try:
                self._take(self._queue.get(timeout=0.2))
            except queue.Empty:
                continue


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--case", required=True, help="case source directory")
    ap.add_argument("--work", required=True, help="build-tree copies the program was built from")
    ap.add_argument("--build-dir", help="CMake build tree, for {build_dir} in host_args")
    ap.add_argument("--target", help="the program's CMake target, for {target} in host_args")
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
    state_file = os.path.join(args.work, "state")
    env = dict(os.environ, HOTPP_E2E_STOP=stop_file, HOTPP_E2E_STATE=state_file, **spec.get("env", {}))
    expand = lambda a: a.format(build_dir=args.build_dir, target=args.target)
    host_args = [expand(a) for a in spec.get("host_args", [])]
    popen = dict(stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, start_new_session=True)
    procs = []
    host_cmd = None
    if spec.get("serve"):
        runtime = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
        endpoint = os.path.join(runtime, f"hotpp-e2e-{os.getpid()}.sock")
        host_cmd = [args.host, *host_args, "--endpoint", endpoint]
        procs.append(subprocess.Popen(host_cmd, env=env, **popen))
        time.sleep(0.3)
        env.update(HOTPP_ENDPOINT=endpoint, HOTPP_LAUNCH_HOST="0")
        procs.append(subprocess.Popen([args.exe], env=env, **popen))
    elif "gdb" in spec:
        runtime = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
        endpoint = os.path.join(runtime, f"hotpp-e2e-{os.getpid()}.sock")
        procs.append(subprocess.Popen([args.host, *host_args, "--endpoint", endpoint, "--once"], env=env, **popen))
        time.sleep(0.3)  # let the host start listening
        env.update(HOTPP_ENDPOINT=endpoint, HOTPP_LAUNCH_HOST="0")
        script = os.path.join(args.case, spec["gdb"])
        procs.append(subprocess.Popen(["gdb", "-q", "-batch", "-nx", "-iex", "set debuginfod enabled off",
                                       "-x", script, "--args", args.exe], env=env, **popen))
    else:
        procs.append(subprocess.Popen([args.host, *host_args, "--", args.exe], env=env, **popen))
    out = Output(procs)
    error = None
    try:
        t = lambda: time.monotonic() + args.step_timeout
        pos = out.wait_for(r"^\[hotpp\] tracking ", 0, t())  # dependency scan done
        pos = max(pos, out.wait_for(r"^frame 2$", 0, t()))
        for n, step in enumerate(spec["steps"], 1):
            print(f"--- step {n}: {step.get('name', '')}", flush=True)
            if "restart_host" in step:
                host = procs[0]
                host.send_signal(getattr(signal, "SIG" + step["restart_host"]))
                host.wait(timeout=15)
                procs[0] = subprocess.Popen(host_cmd, env=env, **popen)
                out.add(procs[0])
            for dst, src in step.get("copy", {}).items():
                shutil.copyfile(os.path.join(args.case, src), os.path.join(args.work, dst))
            expect = step["expect"]
            if step.get("from_start"):  # what the step waits for may already have happened
                pos = 0
            for pattern in expect if isinstance(expect, list) else [expect]:
                pos = out.wait_for(pattern, pos, t())
            pos = out.wait_for(r"^frame \d+$", pos, t(), count=settle)
        for pattern in spec.get("expect_output", []):
            pos = out.wait_for(pattern, pos, t())
        open(stop_file, "w").close()
        if spec.get("serve"):
            procs[1].wait(timeout=15)        # the program finishes on its own
            procs[0].send_signal(signal.SIGTERM)  # then the host is stopped
        out.drain(15)
        for proc in procs:
            code = proc.wait(timeout=15)
            if code != 0 and not error:
                error = f"{proc.args[0]} exited with {code}"
        if not error and not any(line.startswith("PASS") for line in out.lines):
            error = "the program did not report PASS"
    except RuntimeError as e:
        error = str(e)
    finally:
        for proc in procs:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
        for path, (content, st) in originals.items():
            with open(path, "wb") as f:
                f.write(content)
            if not spec.get("rebuild_after"):
                os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
        if spec.get("rebuild_after"):
            # The case rebuilt the program from a later version; build it from v1 again.
            build = subprocess.run(expand("cmake --build {build_dir} --target {target}"), shell=True,
                                   capture_output=True, text=True)
            if build.returncode != 0 and not error:
                error = "rebuilding the program from v1 failed:\n" + build.stdout + build.stderr
        for f in (stop_file, state_file):
            if os.path.exists(f):
                os.remove(f)

    if error:
        print(f"error: {error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
