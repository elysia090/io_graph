#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
	cat >&2 <<'USAGE'
usage: run_kernel_bench_matrix.sh BENCH ARTIFACT_DIR [OUT_DIR]

Runs the kernel selftests io_graph benchmark matrix against prebuilt policy
artifacts in ARTIFACT_DIR. Expected artifact stems:

  typical_100 typical_1000 typical_10000
  shared_1000 shared_10000
  long_1000 long_10000
  typical_1000_entries16 typical_1000_entries64
  typical_1000_entries128 typical_1000_entries256

The script writes per-row logs and matrix.tsv to OUT_DIR.
USAGE
	exit 2
fi

bench=$1
artifacts=$2
out=${3:-"$artifacts/matrix-current"}
warmup=${WARMUP:-1}
duration=${DURATION:-5}
early_miss=${EARLY_MISS:-/x/miss/early-00/not-matched}
entry_counts=${ENTRY_COUNTS:-"1 16 64"}

mkdir -p "$out"
summary_tsv="$out/matrix.tsv"
printf 'name\tdataset\tprefixes\tcase\tbench\tops_mps\tstddev_mps\tns_op\tlog\n' > "$summary_tsv"

selector_for()
{
	head -n 1 "$artifacts/$1.txt"
}

probe_len_for()
{
	sed -n 's/.*"max_probe_len": \([0-9][0-9]*\).*/\1/p' "$artifacts/$1.meta.json"
}

run_row()
{
	local name=$1 dataset=$2 prefixes=$3 case=$4 bench_name=$5
	shift 5
	local log="$out/$name.log"
	local summary tail ops stddev ns

	printf 'RUN\t%s\t%s\t%s\t%s\t%s\n' "$name" "$dataset" "$prefixes" "$case" "$bench_name" >&2
	"$bench" -w "$warmup" -d "$duration" "$bench_name" "$@" > "$log"
	summary=$(grep '^Summary:' "$log" | tail -n 1)
	tail=${summary#*total operations}
	ops=$(printf '%s\n' "$tail" | awk '{print $1}')
	stddev=$(printf '%s\n' "$tail" | awk '{print $3}' | sed 's/M\/s//')
	ns=$(awk -v m="$ops" 'BEGIN { if (m > 0) printf "%.2f", 1000.0 / m; else printf "nan" }')
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$name" "$dataset" "$prefixes" "$case" "$bench_name" \
		"$ops" "$stddev" "$ns" "$log" >> "$summary_tsv"
	printf 'SUMMARY\t%s\t%s M/s\t%s ns/op\n' "$name" "$ops" "$ns" >&2
}

run_row_c1()
{
	local name=$1 dataset=$2 prefixes=$3 case=$4 bench_name=$5
	shift 5
	local log="$out/$name.log"
	local summary tail ops stddev ns

	printf 'RUN\t%s\t%s\t%s\t%s\t%s\tconsumer=1\n' "$name" "$dataset" "$prefixes" "$case" "$bench_name" >&2
	"$bench" -w "$warmup" -d "$duration" -c 1 "$bench_name" "$@" > "$log"
	summary=$(grep '^Summary:' "$log" | tail -n 1)
	tail=${summary#*total operations}
	ops=$(printf '%s\n' "$tail" | awk '{print $1}')
	stddev=$(printf '%s\n' "$tail" | awk '{print $3}' | sed 's/M\/s//')
	ns=$(awk -v m="$ops" 'BEGIN { if (m > 0) printf "%.2f", 1000.0 / m; else printf "nan" }')
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$name" "$dataset" "$prefixes" "$case" "$bench_name" \
		"$ops" "$stddev" "$ns" "$log" >> "$summary_tsv"
	printf 'SUMMARY\t%s\t%s M/s\t%s ns/op\n' "$name" "$ops" "$ns" >&2
}

run_policy_rows()
{
	local stem=$1 dataset=$2 prefixes=$3 selector probe

	selector=$(selector_for "$stem")
	probe=$(probe_len_for "$stem")
	run_row "${stem}_compact_hit" "$dataset" "$prefixes" hit \
		iograph-compact-decision \
		--blob "$artifacts/$stem.iog" --selector "$selector"
	run_row "${stem}_compact_drop" "$dataset" "$prefixes" drop \
		iograph-compact-prefilter \
		--blob "$artifacts/$stem.iog" --selector "$selector" --drop-action 1
	run_row "${stem}_lpm_full_drop" "$dataset" "$prefixes" drop \
		iograph-lpm-prefilter \
		--prefixes "$artifacts/$stem.txt" --selector "$selector" --drop-action 1
	run_row "${stem}_lpm_bounded_drop" "$dataset" "$prefixes" drop \
		iograph-lpm-bounded-prefilter \
		--prefixes "$artifacts/$stem.txt" --selector "$selector" --drop-action 1
	run_row "${stem}_lpm_full_hit" "$dataset" "$prefixes" hit \
		iograph-lpm-decision \
		--prefixes "$artifacts/$stem.txt" --selector "$selector"
	run_row "${stem}_lpm_bounded_hit" "$dataset" "$prefixes" hit \
		iograph-lpm-bounded-decision \
		--prefixes "$artifacts/$stem.txt" --selector "$selector"
	run_row "${stem}_compact_acquire_hit" "$dataset" "$prefixes" acquire_hit \
		iograph-compact-acquire-decision \
		--blob "$artifacts/$stem.iog" --selector "$selector" --probe-len "$probe"
	run_row "${stem}_lpm_bounded_acquire_hit" "$dataset" "$prefixes" acquire_hit \
		iograph-lpm-bounded-acquire-decision \
		--prefixes "$artifacts/$stem.txt" --selector "$selector" --probe-len "$probe"
}

run_early_rows()
{
	local stem=$1 dataset=$2 prefixes=$3 probe

	probe=$(probe_len_for "$stem")
	run_row "${stem}_compact_early_miss" "$dataset" "$prefixes" early_miss \
		iograph-compact-decision \
		--blob "$artifacts/$stem.iog" --selector "$early_miss"
	run_row "${stem}_lpm_bounded_early_miss" "$dataset" "$prefixes" early_miss \
		iograph-lpm-bounded-decision \
		--prefixes "$artifacts/$stem.txt" --selector "$early_miss"
	run_row "${stem}_compact_acquire_early_miss" "$dataset" "$prefixes" acquire_early_miss \
		iograph-compact-acquire-decision \
		--blob "$artifacts/$stem.iog" --selector "$early_miss" --probe-len "$probe"
	run_row "${stem}_lpm_bounded_acquire_early_miss" "$dataset" "$prefixes" acquire_early_miss \
		iograph-lpm-bounded-acquire-decision \
		--prefixes "$artifacts/$stem.txt" --selector "$early_miss" --probe-len "$probe"
}

run_payload_rows()
{
	local stem=$1 dataset=$2 prefixes=$3 selector payload

	selector=$(selector_for "$stem")
	for payload in 300 800 2048; do
		run_row_c1 "${stem}_compact_post_${payload}" "$dataset" "$prefixes" "post_${payload}" \
			iograph-compact-post-payload \
			--blob "$artifacts/$stem.iog" --selector "$selector" --drop-action 2 --payload-size "$payload"
		run_row_c1 "${stem}_always_post_${payload}" "$dataset" "$prefixes" "always_post_${payload}" \
			iograph-ringbuf-always-post \
			--selector "$selector" --payload-size "$payload"
	done
}

run_discard_row()
{
	local stem=$1 dataset=$2 prefixes=$3 selector

	selector=$(selector_for "$stem")
	run_row "${stem}_discard_after_reserve" "$dataset" "$prefixes" discard_after_reserve \
		iograph-discard-after-reserve \
		--blob "$artifacts/$stem.iog" --selector "$selector" --drop-action 1
}

run_entry_rows()
{
	local entries=$1 blob last selector

	if [ "$entries" -eq 1 ]; then
		blob="$artifacts/typical_1000.iog"
	else
		blob="$artifacts/typical_1000_entries${entries}.iog"
	fi
	last=$((entries - 1))
	selector=$(selector_for typical_1000)
	run_row "entries${entries}_entry_id" multi "$entries" entry_id \
		iograph-compact-decision \
		--blob "$blob" --selector "$selector" --entry-id "$last"
	run_row "entries${entries}_entry_idx" multi "$entries" entry_idx \
		iograph-compact-idx-decision \
		--blob "$blob" --selector "$selector" --entry-idx "$last"
}

run_row floor floor 0 empty iograph-hook-floor --selector "$early_miss"

run_policy_rows typical_100 typical 100
run_policy_rows typical_1000 typical 1000
run_policy_rows typical_10000 typical 10000
run_policy_rows shared_1000 shared-prefix 1000
run_policy_rows shared_10000 shared-prefix 10000
run_policy_rows long_1000 long-path 1000
run_policy_rows long_10000 long-path 10000

run_early_rows typical_1000 typical 1000
run_early_rows typical_10000 typical 10000

run_discard_row typical_1000 typical 1000
run_discard_row typical_10000 typical 10000

run_payload_rows typical_1000 typical 1000
run_payload_rows typical_10000 typical 10000

for entries in $entry_counts; do
	run_entry_rows "$entries"
done

printf 'matrix_tsv=%s\n' "$summary_tsv"
