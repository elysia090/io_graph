#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")"

counts="${IOG_COUNTS:-100,1000}"

echo "native_linux_validation=io_graph"
printf 'uname=%s\n' "$(uname -a)"

if [ -r /proc/sys/kernel/perf_event_paranoid ]; then
	printf 'perf_event_paranoid=%s\n' \
		"$(cat /proc/sys/kernel/perf_event_paranoid)"
fi

if [ -d /sys/bus/event_source/devices/cpu ]; then
	printf 'pmu_cpu_type=%s\n' \
		"$(cat /sys/bus/event_source/devices/cpu/type 2>/dev/null || printf 'unavailable')"
else
	printf 'pmu_cpu_type=unavailable\n'
fi

make clean all

if [ -n "${IOG_ITERS:-}" ]; then
	./iog_bench --counts "$counts" --iters "$IOG_ITERS"
else
	./iog_bench --counts "$counts"
fi
