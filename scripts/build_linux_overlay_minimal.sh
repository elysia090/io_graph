#!/usr/bin/env sh
set -eu

usage()
{
	printf '%s\n' \
		'usage: build_linux_overlay_minimal.sh [--selftests-bench] LINUX_TREE BUILD_DIR [SELFTESTS_OUTPUT]' \
		'' \
		'Incrementally build only the io_graph Linux overlay pieces.' \
		'' \
		'Default:' \
		'  - build kernel/bpf/iograph_map.o' \
		'  - build kernel/bpf/iograph_kfunc.o' \
		'' \
		'With --selftests-bench:' \
		'  - additionally build tools/testing/selftests/bpf bench into SELFTESTS_OUTPUT' \
		'    or BUILD_DIR/selftests-bpf when SELFTESTS_OUTPUT is omitted.' \
		'' \
		'This script does not build bzImage, install modules, edit .wslconfig, or boot a' \
		'kernel. It is the fast validation gate for overlay source changes.' >&2
}

selftests_bench=0
while [ "$#" -gt 0 ]; do
	case "$1" in
	--selftests-bench)
		selftests_bench=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	--)
		shift
		break
		;;
	-*)
		usage
		exit 2
		;;
	*)
		break
		;;
	esac
done

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
	usage
	exit 2
fi

linux_tree=$1
build_dir=$2
selftests_output=${3:-"$build_dir/selftests-bpf"}

make_cmd=${MAKE:-make}
jobs=${JOBS:-}
if [ -z "$jobs" ]; then
	jobs=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
fi

if ! command -v "$make_cmd" >/dev/null 2>&1; then
	printf 'missing make command: %s\n' "$make_cmd" >&2
	printf 'enter the kernel build shell or pass MAKE=/path/to/make\n' >&2
	exit 1
fi

git -C "$linux_tree" rev-parse --show-toplevel >/dev/null

for file in \
	"$linux_tree/kernel/bpf/iograph_map.c" \
	"$linux_tree/kernel/bpf/iograph_kfunc.c" \
	"$linux_tree/kernel/bpf/iograph_internal.h" \
	"$linux_tree/include/uapi/linux/bpf_iograph.h" \
	"$linux_tree/tools/include/uapi/linux/bpf_iograph.h"
do
	if [ ! -f "$file" ]; then
		printf 'missing overlay file: %s\n' "$file" >&2
		exit 1
	fi
done

mkdir -p "$build_dir"

"$make_cmd" -C "$linux_tree" O="$build_dir" -j"$jobs" \
	kernel/bpf/iograph_map.o \
	kernel/bpf/iograph_kfunc.o

if [ "$selftests_bench" -eq 1 ]; then
	mkdir -p "$selftests_output"
	"$make_cmd" -C "$linux_tree/tools/testing/selftests/bpf" \
		OUTPUT="$selftests_output" -j"$jobs" bench
fi
