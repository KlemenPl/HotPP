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
        # GitHub shows about 4 KB of an annotation: the failure lines first, then
        # as much of the end of the output as still fits.
        key = [l for l in lines if re.match(r"^(FAIL|error:|Traceback)", l)][-5:]
        head = "\n".join(key)[-1500:]
        tail = "\n".join(lines[-80:])[-(3800 - len(head)):]
        text = (head + "\n----- end of output:\n" + tail) if head else tail
        print(f"::error title={escape(case.get('name', 'test'))} failed::{escape(text)}")
    print(f"{failed} failed test(s) reported")
    return 1


if __name__ == "__main__":
    sys.exit(main())
