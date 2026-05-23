#!/usr/bin/env sh
set -eu

if [ "$#" -ne 1 ]; then
	printf 'usage: %s LINUX_TREE\n' "$0" >&2
	exit 2
fi

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
linux_tree=$1

git -C "$linux_tree" rev-parse --show-toplevel >/dev/null

if [ -n "$(git -C "$linux_tree" status --porcelain=v1)" ]; then
	printf 'linux tree is dirty; use a disposable clean worktree\n' >&2
	exit 1
fi

git -C "$linux_tree" apply --check "$repo/kernel/linux.integration.patch"

install -D -m 0644 "$repo/kernel/include/uapi/linux/bpf_iograph.h" \
	"$linux_tree/include/uapi/linux/bpf_iograph.h"
install -D -m 0644 "$repo/kernel/include/uapi/linux/bpf_iograph.h" \
	"$linux_tree/tools/include/uapi/linux/bpf_iograph.h"
install -D -m 0644 "$repo/kernel/bpf/iograph_internal.h" \
	"$linux_tree/kernel/bpf/iograph_internal.h"
install -D -m 0644 "$repo/kernel/bpf/iograph_map.c" \
	"$linux_tree/kernel/bpf/iograph_map.c"
install -D -m 0644 "$repo/kernel/bpf/iograph_kfunc.c" \
	"$linux_tree/kernel/bpf/iograph_kfunc.c"

for file in "$repo"/kernel/selftests/bpf/progs/*; do
	install -D -m 0644 "$file" \
		"$linux_tree/tools/testing/selftests/bpf/progs/$(basename "$file")"
done
for file in "$repo"/kernel/selftests/bpf/prog_tests/*; do
	install -D -m 0644 "$file" \
		"$linux_tree/tools/testing/selftests/bpf/prog_tests/$(basename "$file")"
done
for file in "$repo"/kernel/selftests/bpf/benchs/*; do
	install -D -m 0644 "$file" \
		"$linux_tree/tools/testing/selftests/bpf/benchs/$(basename "$file")"
	done

git -C "$linux_tree" apply "$repo/kernel/linux.integration.patch"
git -C "$linux_tree" diff --check
