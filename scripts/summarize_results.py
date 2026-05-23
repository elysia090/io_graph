#!/usr/bin/env python3
import sys


def main() -> int:
    for line in sys.stdin:
        if line.startswith(("perf_counters=", "case prefixes=", "| 100 ", "| 1000 ")):
            print(line, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
