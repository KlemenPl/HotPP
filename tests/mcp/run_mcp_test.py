#!/usr/bin/env python3
"""End-to-end test of hotpp mcp, driven the way an AI agent would.

A minimal MCP client talks JSON-RPC to `hotpp mcp` over stdio. It launches the
test program (tests/e2e/cases/mcp), edits its build-tree sources and checks what
the tools report: patches applied as one, compile errors, restart-required
changes, a rebuild and restart, output and logs, malformed input. Then it
attaches to a host started the usual way (`hotpp -- program`), and checks that a
launched program dies with the MCP server even when the server is killed.

Hosts are found in $XDG_RUNTIME_DIR (Windows: by pipe name), so the test uses a
private one: other tests' hosts running in parallel stay out of it. With
--emulator (a cross build), hotpp runs under it, e.g. Wine.
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
import tempfile
import threading
import time


# Processes start in a session of their own (POSIX), so they can be killed with
# everything they started.
NEW_SESSION = {"start_new_session": True} if os.name == "posix" else {}


def kill_tree(proc):
    if os.name == "posix":
        os.killpg(proc.pid, signal.SIGKILL)
    else:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)


class Failure(Exception):
    pass


def check(cond, what, detail=""):
    if not cond:
        raise Failure(what + (("\n" + detail) if detail else ""))


class Client:
    """A minimal MCP client: requests may overlap; replies are matched by id."""

    def __init__(self, cmd, env):
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=sys.stderr,
                                     env=env, text=True, bufsize=1, **NEW_SESSION)
        self.replies = {}
        self.unmatched = []  # replies with a null id (parse errors)
        self.progress = []
        self.cv = threading.Condition()
        self.next_id = 1
        self.closed = False
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                print(f"NOT JSON on stdout: {line!r}")
                msg = {"__garbage__": line}
            with self.cv:
                if isinstance(msg, list):
                    for m in msg:
                        self.replies[m.get("id")] = m
                elif msg.get("method") == "notifications/progress":
                    self.progress.append(msg["params"])
                elif "id" in msg and msg["id"] is not None:
                    self.replies[msg["id"]] = msg
                else:
                    self.unmatched.append(msg)
                self.cv.notify_all()
        with self.cv:
            self.closed = True
            self.cv.notify_all()

    def send_raw(self, text):
        self.proc.stdin.write(text + "\n")
        self.proc.stdin.flush()

    def send(self, method, params=None, notify=False):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        rid = None
        if not notify:
            with self.cv:
                rid = self.next_id
                self.next_id += 1
            msg["id"] = rid
        self.send_raw(json.dumps(msg))
        return rid

    def wait(self, rid, timeout=120):
        deadline = time.monotonic() + timeout
        with self.cv:
            while rid not in self.replies:
                check(not self.closed, f"server exited while waiting for reply {rid}")
                left = deadline - time.monotonic()
                check(left > 0, f"no reply to request {rid} within {timeout} s")
                self.cv.wait(left)
            return self.replies.pop(rid)

    def request(self, method, params=None, timeout=120):
        return self.wait(self.send(method, params), timeout)

    def tool(self, name, timeout=300, **args):
        r = self.request("tools/call", {"name": name, "arguments": args, "_meta": {"progressToken": name}}, timeout)
        check("result" in r, f"{name}: protocol error", json.dumps(r))
        res = r["result"]
        text = res["content"][0]["text"]
        print(f"--- {name} {json.dumps(args)} -> isError={res['isError']}\n{text}\n")
        return res

    def close(self, timeout=20):
        self.proc.stdin.close()
        return self.proc.wait(timeout)


def text_of(res):
    return res["content"][0]["text"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--case", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--emulator", default="", help="command (parts separated by |) that runs hotpp: wine, for cross builds")
    args = ap.parse_args()
    emulator = [a for a in args.emulator.split("|") if a]
    # A Windows build (native or under an emulator): pipes instead of sockets, and a
    # hot restart continues as a new process.
    windows = args.exe.endswith(".exe")

    v1_dir = os.path.join(args.case, "v1")
    originals = {}
    for name in os.listdir(v1_dir):
        path = os.path.join(args.work, name)
        with open(os.path.join(v1_dir, name), "rb") as f:
            v1 = f.read()
        with open(path, "rb") as f:
            if f.read() != v1:
                print(f"error: {path} does not match v1; rebuild first")
                return 1
        originals[path] = (v1, os.stat(path))

    def put(version, name):
        # A rename, as most editors save.
        dst = os.path.join(args.work, name)
        tmp = dst + ".tmp"
        shutil.copyfile(os.path.join(args.case, version, name), tmp)
        os.replace(tmp, dst)

    runtime = tempfile.mkdtemp(prefix="hotpp-mcp-test-")
    os.chmod(runtime, 0o700)
    stop_file = os.path.join(args.work, "stop")
    if os.path.exists(stop_file):
        os.remove(stop_file)
    env = dict(os.environ, XDG_RUNTIME_DIR=runtime)
    if windows:
        env["HOTPP_ENDPOINT_PREFIX"] = rf"\\.\pipe\hotpp-mcp-test-{os.getpid()}"
    env.pop("HOTPP_ENDPOINT", None)
    build = f"cmake --build {args.build_dir} --target {args.target}"
    mcp_cmd = [*emulator, args.host, "mcp", "--structured", "--build", build]
    rebuilt = False
    others = []
    client = None
    error = None
    try:
        client = Client(mcp_cmd, env)

        # --- lifecycle ----------------------------------------------------------
        r = client.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                          "clientInfo": {"name": "run_mcp_test", "version": "1"}})
        res = r["result"]
        check(res["protocolVersion"] == "2025-11-25", "protocol version not echoed", json.dumps(res))
        check(res["serverInfo"]["name"] == "hotpp", "server name")
        check("tools" in res["capabilities"], "tools capability")
        check("hotpp_reload" in res.get("instructions", ""), "instructions mention hotpp_reload")
        client.send("notifications/initialized", notify=True)
        r = client.request("initialize", {"protocolVersion": "1999-01-01"})
        check(r["result"]["protocolVersion"] == "2025-11-25", "unknown version answered with the newest")
        tools = {t["name"]: t for t in client.request("tools/list")["result"]["tools"]}
        expected = {"hotpp_status", "hotpp_reload", "hotpp_launch", "hotpp_restart", "hotpp_stop", "hotpp_output",
                    "hotpp_logs", "hotpp_set_auto_reload"}
        check(set(tools) == expected, "tool list", str(sorted(tools)))
        for t in tools.values():
            check(t["inputSchema"]["type"] == "object" and t["description"], f"schema of {t['name']}")
        check(tools["hotpp_status"]["annotations"]["readOnlyHint"], "status is read-only")
        check(tools["hotpp_stop"]["annotations"]["destructiveHint"], "stop is destructive")

        res = client.tool("hotpp_status")
        check(not res["isError"] and "No program" in text_of(res), "status with nothing running")
        res = client.tool("hotpp_reload")
        check(res["isError"] and "No program" in text_of(res), "reload with nothing running")

        # --- launch ---------------------------------------------------------------
        res = client.tool("hotpp_launch", command=[args.exe], env={"HOTPP_E2E_STOP": stop_file})
        check(not res["isError"], "launch failed", text_of(res))
        check("Auto-reload is off" in text_of(res), "launch says auto-reload is off")
        pid = res["structuredContent"]["program"]["pid"]
        check(any(p["progressToken"] == "hotpp_launch" for p in client.progress), "launch sent progress")

        cursor = 0

        def wait_output(pattern, timeout=30, since=None):
            """Polls hotpp_output until a line matches; returns (match, cursor after it)."""
            nonlocal cursor
            pos = cursor if since is None else since
            rx = re.compile(pattern)
            deadline = time.monotonic() + timeout
            text = ""
            while time.monotonic() < deadline:
                r = client.request("tools/call", {"name": "hotpp_output", "arguments": {"since": pos}})["result"]
                check(not r["isError"], "hotpp_output failed", text_of(r))
                sc = r["structuredContent"]
                text += sc["text"]
                pos = sc["next"]
                m = rx.search(text)
                if m:
                    cursor = pos
                    return m
                time.sleep(0.1)
            raise Failure(f"output never matched {pattern!r}; last output:\n{text[-2000:]}")

        wait_output(r"value=1 label=one counter=\d+")

        # --- two files, auto-reload off: nothing happens until hotpp_reload --------
        put("v2", "game.cpp")
        put("v2", "extra.cpp")
        deadline = time.monotonic() + 10
        while True:
            st = client.tool("hotpp_status")["structuredContent"]
            session = st["programs"][0]["session"]
            if len(session["pending"]) == 2:
                break
            check(time.monotonic() < deadline, "status never listed the two pending files", json.dumps(session))
            time.sleep(0.2)
        check(not session["in_sync"], "pending files mean not in sync")
        m = wait_output(r"value=(\d) label=(\w+) counter=(\d+)")
        check(m.group(1) == "1", "patched although auto-reload is off")

        res = client.tool("hotpp_reload", pid=pid, observe_ms=600)
        sc = res["structuredContent"]
        check(not res["isError"] and sc["outcome"] == "applied", "reload of two files", text_of(res))
        check("Output over the next 600 ms" in text_of(res) and "value=2 label=two" in text_of(res),
              "observe_ms shows the patched program's output", text_of(res))
        check(len(sc["report"]["files"]) == 2, "both files in one patch", json.dumps(sc["report"]))
        check(sc["in_sync"], "in sync after the patch")
        before = int(m.group(3))
        m = wait_output(r"value=2 label=two counter=(\d+)")
        check(int(m.group(1)) > before, "the counter kept its value across the patch")

        res = client.tool("hotpp_reload")
        check(not res["isError"] and res["structuredContent"]["outcome"] == "up_to_date", "nothing to do")
        t0 = time.monotonic()
        res = client.tool("hotpp_output", wait_ms=400)
        check(time.monotonic() - t0 >= 0.35 and "value=2" in text_of(res), "hotpp_output waits and watches",
              text_of(res))

        # --- a compile error, then the fix ---------------------------------------
        put("broken", "game.cpp")
        res = client.tool("hotpp_reload")
        sc = res["structuredContent"]
        check(res["isError"] and sc["outcome"] == "compile_failed", "compile error reported", text_of(res))
        check("error" in sc["report"]["compiler_output"] and "Compiler output" in text_of(res),
              "compiler output included", text_of(res))
        check("\x1b" not in sc["report"]["compiler_output"], "no color codes in the compiler output")
        check(not sc["in_sync"], "a failed patch means not in sync")
        res = client.tool("hotpp_reload")
        check(res["isError"] and res["structuredContent"]["outcome"] == "out_of_sync", "still failing")
        put("v3", "game.cpp")
        res = client.tool("hotpp_reload")
        check(not res["isError"] and res["structuredContent"]["outcome"] == "applied", "fixed", text_of(res))
        check(res["structuredContent"]["in_sync"], "in sync again")
        wait_output(r"value=3 label=two")

        # --- protocol robustness ---------------------------------------------------
        client.send_raw("this is not json")
        client.send_raw('{"jsonrpc": "2.0", "id": 9001, "method": "no/such/method"}')
        client.send_raw('{"jsonrpc": "2.0", "id": 9002, "method": "tools/call", "params": {"name": "nope"}}')
        client.send_raw('{"jsonrpc": "2.0", "method": "notifications/whatever"}')
        client.send_raw('[{"jsonrpc": "2.0", "id": 9003, "method": "ping"}]')
        check(client.wait(9001)["error"]["code"] == -32601, "unknown method")
        check(client.wait(9002)["error"]["code"] == -32602, "unknown tool")
        check(client.wait(9003)["result"] == {}, "batched ping")
        with client.cv:
            check(any(m.get("error", {}).get("code") == -32700 for m in client.unmatched), "parse error reply")
        res = client.tool("hotpp_reload", timeout_ms="soon")
        check(res["isError"] and "Invalid arguments" in text_of(res), "bad argument type")
        res = client.tool("hotpp_reload", bogus=1)
        check(res["isError"] and "unknown argument" in text_of(res), "unknown argument")
        check(client.request("ping")["result"] == {}, "still answering")

        # --- auto-reload on: a save patches by itself; hotpp_logs sees it ---------
        res = client.tool("hotpp_logs")
        check("applied" in text_of(res), "logs show the patches", text_of(res))
        since = res["structuredContent"]["next"]
        res = client.tool("hotpp_set_auto_reload", enabled=True)
        check(not res["isError"], "auto-reload on")
        rid = client.send("tools/call", {"name": "hotpp_logs",
                                         "arguments": {"since": since, "wait_ms": 1000, "limit": 1}})
        client.wait(rid)  # the auto-reload note itself
        deadline = time.monotonic() + 30
        seen_patch = None
        put("v2", "game.cpp")
        while seen_patch is None:
            check(time.monotonic() < deadline, "no automatic patch showed up in hotpp_logs")
            r = client.request("tools/call", {"name": "hotpp_logs", "arguments": {"since": since, "wait_ms": 5000}})
            sc = r["result"]["structuredContent"]
            since = sc["next"]
            for e in sc["events"]:
                if e["kind"] == "patch":
                    seen_patch = e["patch"]
        check(seen_patch["status"] == "applied" and seen_patch["trigger"] == "watch", "automatic patch",
              json.dumps(seen_patch))
        wait_output(r"value=2 label=two")
        res = client.tool("hotpp_reload")
        sc = res["structuredContent"]
        check(sc["outcome"] == "up_to_date" and any(p["trigger"] == "watch" for p in sc["recent"]),
              "reload reports the automatic patch", text_of(res))
        client.tool("hotpp_set_auto_reload", enabled=False)

        # --- a change that needs a restart, then hotpp_restart ---------------------
        put("v4", "game.cpp")
        res = client.tool("hotpp_reload")
        check(res["isError"] and res["structuredContent"]["outcome"] == "restart_required", "restart required",
              text_of(res))
        check("hotpp_restart" in text_of(res), "suggests hotpp_restart")
        rebuilt = True
        res = client.tool("hotpp_restart")
        sc = res["structuredContent"]
        check(not res["isError"] and sc["outcome"] == "restarted", "restart", text_of(res))
        if windows:
            pid = sc["program"]["pid"]  # a new process: Windows has no exec
        else:
            check(sc["program"]["pid"] == pid, "same pid after the restart")
        wait_output(r"started restarted=1")
        wait_output(r"value=4 label=two")

        res = client.tool("hotpp_stop")
        check(not res["isError"] and "Stopped" in text_of(res), "stop", text_of(res))
        res = client.tool("hotpp_status")
        check("No program" in text_of(res), "nothing left after stop", text_of(res))

        # --- a host started the usual way ------------------------------------------
        user = subprocess.Popen([*emulator, args.host, "--", args.exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                env=dict(env, HOTPP_E2E_STOP=stop_file), text=True, bufsize=1, **NEW_SESSION)
        others.append(user)
        user_lines = queue.Queue()
        threading.Thread(target=lambda: [user_lines.put(l) for l in user.stdout], daemon=True).start()

        def wait_user(pattern, timeout=40):
            rx = re.compile(pattern)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                try:
                    line = user_lines.get(timeout=0.5)
                except queue.Empty:
                    continue
                if rx.search(line):
                    return line
            raise Failure(f"the user's host never printed {pattern!r}")

        wait_user(r"^\[hotpp\] watching")
        res = client.tool("hotpp_status")
        progs = res["structuredContent"]["programs"]
        check(len(progs) == 1 and not progs[0]["launched"], "status sees the user's program", text_of(res))
        user_pid = progs[0]["pid"]
        res = client.tool("hotpp_output")
        check(res["isError"] and "isn't captured" in text_of(res), "no output capture for user programs")
        res = client.tool("hotpp_stop")
        check(res["isError"] and "left alone" in text_of(res), "won't stop a user's program")
        res = client.tool("hotpp_set_auto_reload", enabled=False)
        check(not res["isError"] and "until hotpp mcp exits" in text_of(res), "temporary auto-reload setting")
        wait_user(r"auto-reload off")
        put("v5", "game.cpp")
        time.sleep(0.5)
        res = client.tool("hotpp_reload", pid=user_pid)
        check(not res["isError"] and res["structuredContent"]["outcome"] == "applied", "reload on the user's host",
              text_of(res))
        wait_user(r"value=5")
        check(client.close() == 0, "hotpp mcp exit code")
        client = None
        wait_user(r"auto-reload on")  # reverted when the controller went away
        open(stop_file, "w").close()
        wait_user(r"^PASS")
        check(user.wait(20) == 0, "the user's program exit code")
        os.remove(stop_file)

        # --- killing hotpp mcp stops what it launched -------------------------------
        client = Client(mcp_cmd, env)
        client.request("initialize", {"protocolVersion": "2025-06-18"})
        res = client.tool("hotpp_launch", command=[args.exe])
        check(not res["isError"], "second launch", text_of(res))
        child = res["structuredContent"]["program"]["pid"]
        host_pid = res["structuredContent"]["host"]["pid"]
        client.proc.kill()
        client.proc.wait(10)
        client = None
        deadline = time.monotonic() + 10
        while windows:
            # The pids are Windows ones: ask a fresh hotpp mcp what still runs.
            probe = Client(mcp_cmd, env)
            probe.request("initialize", {"protocolVersion": "2025-06-18"})
            gone = "No program" in text_of(probe.tool("hotpp_status"))
            probe.close()
            if gone:
                break
            check(time.monotonic() < deadline, "still running after hotpp mcp was killed")
            time.sleep(0.5)
        while not windows:
            alive = []
            for p in (child, host_pid):
                try:
                    os.kill(p, 0)
                    with open(f"/proc/{p}/stat") as f:
                        if f.read().split(") ")[-1].startswith("Z"):
                            continue  # a zombie: dead, waiting to be reaped
                    alive.append(p)
                except (ProcessLookupError, FileNotFoundError):
                    pass
            if not alive:
                break
            check(time.monotonic() < deadline, f"still running after hotpp mcp was killed: {alive}")
            time.sleep(0.2)
    except Failure as e:
        error = str(e)
    finally:
        if client is not None:
            try:
                client.proc.stdin.close()
                client.proc.wait(20)
            except Exception:
                client.proc.kill()
        for p in others:
            if p.poll() is None:
                kill_tree(p)
                p.wait()
        for path, (content, st) in originals.items():
            with open(path, "wb") as f:
                f.write(content)
            if not rebuilt:
                os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
        for extra in ("game.cpp.tmp", "extra.cpp.tmp", "stop"):
            p = os.path.join(args.work, extra)
            if os.path.exists(p):
                os.remove(p)
        if rebuilt:
            b = subprocess.run(build, shell=True, capture_output=True, text=True)
            if b.returncode != 0 and not error:
                error = "rebuilding the program from v1 failed:\n" + b.stdout + b.stderr
        shutil.rmtree(runtime, ignore_errors=True)
    if error:
        print("FAIL: " + error)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
