#!/usr/bin/env python3
import subprocess


def main() -> int:
    return subprocess.call(["tools/iog_bench/iog_bench", "--counts", "100,1000"])


if __name__ == "__main__":
    raise SystemExit(main())
