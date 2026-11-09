#!/usr/bin/env python3
"""Turns the failed tests in a ctest JUnit report into GitHub Actions error
annotations (the end of each one's output), so failures can be read without
signing in to view logs:

    ctest --test-dir build --output-junit junit.xml || python3 tools/ci_report.py build/junit.xml
"""
import re
import sys
import xml.etree.ElementTree as ET


def escape(s: str) -> str:
    return s.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def main() -> int:
    try:
        root = ET.parse(sys.argv[1]).getroot()
    except (OSError, ET.ParseError, IndexError) as e:
        print(f"::error title=ci_report::can't read the test report: {e}")
        return 1
    failed = 0
    for case in root.iter("testcase"):
        if case.find("failure") is None and case.get("status") not in ("fail", "failed"):
            continue
        failed += 1
        out = case.findtext("system-out") or ""
        # The programs' per-frame lines say nothing; keep the rest.
        noise = r"^(frame \d+|value .*|\s+\[\s*\d+\] .*|  (exe|compile commands|modules|patch regions).*|\s+[0-9a-f]{16}-[0-9a-f]{16} .*)$"
        lines = [l for l in out.splitlines() if l.strip() and not re.match(noise, l)]
        tail = "\n".join(lines[-80:])[-6000:]
        print(f"::error title={escape(case.get('name', 'test'))} failed::{escape(tail)}")
    print(f"{failed} failed test(s) reported")
    return 1


if __name__ == "__main__":
    sys.exit(main())
