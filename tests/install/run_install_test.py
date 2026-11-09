#!/usr/bin/env python3
"""Installs HotPP from a build tree into a temporary prefix, builds a separate
project against it with find_package(HotPP), and runs it under the installed host.
The program launches nothing itself: it connects to the host started here."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    sys.stdout.write(p.stdout[-4000:])
    sys.stdout.write(p.stderr[-4000:])
    if p.returncode != 0:
        raise SystemExit(f"error: exit code {p.returncode}")
    return p.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--consumer", required=True)
    ap.add_argument("--cxx", required=True)
    ap.add_argument("--generator", default="", help="the CMake generator the build used")
    ap.add_argument("--build-type", default="", help="the build's CMAKE_BUILD_TYPE (MSVC runtimes must match)")
    args = ap.parse_args()
    exe = ".exe" if os.name == "nt" else ""
    tmp = tempfile.mkdtemp(prefix="hotpp-install-")
    try:
        prefix = os.path.join(tmp, "prefix")
        run(["cmake", "--install", args.build_dir, "--prefix", prefix])
        for f in ("bin/hotpp" + exe, "include/hotpp/hotpp.h", "share/hotpp/single_include/hotpp/hotpp.h",
                  "lib/cmake/HotPP/HotPPConfig.cmake"):
            if not os.path.exists(os.path.join(prefix, f)):
                raise SystemExit(f"error: {f} was not installed")
        build = os.path.join(tmp, "build")
        generator = ["-G", args.generator] if args.generator else []
        build_type = [f"-DCMAKE_BUILD_TYPE={args.build_type}"] if args.build_type else []
        run(["cmake", "-S", args.consumer, "-B", build, *generator, *build_type, f"-DCMAKE_PREFIX_PATH={prefix}",
             f"-DCMAKE_CXX_COMPILER={args.cxx}"])
        run(["cmake", "--build", build])
        out = run([os.path.join(prefix, "bin", "hotpp" + exe), "--", os.path.join(build, "consumer" + exe), "5"])
        if "agent connected" not in out or "frame 4 value 1" not in out:
            raise SystemExit("error: the consumer did not run connected to the installed host")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
