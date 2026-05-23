#!/usr/bin/env python3
import sys

roots = [
    "/var/lib/kubelet/pods/",
    "/var/log/containers/",
    "/opt/acme/cache/",
    "/srv/app/run/",
]
services = ["api", "worker", "sched", "ingest", "index", "proxy", "bill", "auth"]


def main() -> int:
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    for i in range(count):
        print(f"{roots[i % len(roots)]}t{i % 32:02d}/n{(i // 32) % 8:02d}/"
              f"{services[(i // 5) % len(services)]}/o{i % 1024:04d}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
