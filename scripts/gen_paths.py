#!/usr/bin/env python3
import sys


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else "examples/prefixes_100.txt"
    with open(src, encoding="utf-8") as prefixes:
        for prefix in prefixes:
            prefix = prefix.strip()
            if prefix:
                print(f"{prefix}event.log")
                print(f"{prefix}late-miss")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
