#!/usr/bin/env python3
import argparse


ROOTS = [
    "/var/lib/kubelet/pods/",
    "/var/log/containers/",
    "/opt/acme/cache/",
    "/srv/app/run/",
]
SERVICES = ["api", "worker", "sched", "ingest", "index", "proxy", "bill", "auth"]


def typical(i: int) -> str:
    return (f"{ROOTS[i % len(ROOTS)]}t{i % 32:02d}/n{(i // 32) % 8:02d}/"
            f"{SERVICES[(i // 5) % len(SERVICES)]}/o{i % 1024:04d}/")


def shared_prefix(i: int) -> str:
    return ("/srv/shared/tenant/default/namespace/prod/runtime/"
            "container/worker/shared/component/"
            f"shard-{i:05d}/tail/")


def long_path(i: int) -> str:
    return ("/var/lib/kubelet/pods/tenant-long/namespace-long/"
            "volume-subpaths/runtime/security/observability/"
            "container-rootfs/application/component/args/"
            f"segment-a/segment-b/segment-c/segment-d/object-{i:05d}/")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("count", nargs="?", type=int, default=100)
    parser.add_argument("--dataset", choices=("typical", "shared-prefix",
                                              "long-path"),
                        default="typical")
    args = parser.parse_args()

    makers = {
        "typical": typical,
        "shared-prefix": shared_prefix,
        "long-path": long_path,
    }
    make = makers[args.dataset]
    for i in range(args.count):
        print(make(i))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
