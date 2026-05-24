# Native Kernel Measurement Template

Run label:
io_graph commit:
Linux source:
Linux base tag or commit:
Linux overlay commit:
Build output directory:
Boot artifact directory:

## Environment

```sh
date -Is
uname -a
cat /proc/version
cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo na
ls -l /sys/bus/event_source/devices || true
cat /sys/bus/event_source/devices/cpu/type 2>/dev/null || echo "cpu PMU unavailable"
```

WSL boot artifacts, if applicable:

```powershell
Get-FileHash $env:USERPROFILE\.wslconfig
Get-FileHash C:\path\to\wsl-kernels\io_graph\bzImage
Get-FileHash C:\path\to\wsl-kernels\io_graph\modules.vhdx
```

## Tree State

```sh
git -C ~/src/io_graph rev-parse HEAD
git -C ~/src/wsl2-linux-iograph rev-parse HEAD
git -C ~/src/wsl2-linux-iograph status --short
git -C ~/src/wsl2-linux-iograph diff --stat HEAD~1..HEAD
```

The Linux measurement tree must be disposable. Do not apply the overlay to the
pristine base clone.

## Bench Commands

```sh
cd ~/src/wsl2-linux-iograph/tools/testing/selftests/bpf

./bench -w 1 -d 5 iograph-hook-floor \
	--selector /drop/event

./bench -w 1 -d 5 iograph-compact-decision \
	--blob /path/to/policy-100.iog \
	--selector /drop/event

./bench -w 1 -d 5 iograph-compact-prefilter \
	--blob /path/to/policy-100.iog \
	--selector /drop/event \
	--drop-action 1

./bench -w 1 -d 5 iograph-lpm-decision \
	--prefixes /path/to/prefixes-100.txt \
	--selector /drop/event

./bench -w 1 -d 5 iograph-lpm-bounded-decision \
	--prefixes /path/to/prefixes-100.txt \
	--selector /drop/event

./bench -w 1 -d 5 iograph-lpm-prefilter \
	--prefixes /path/to/prefixes-100.txt \
	--selector /drop/event \
	--drop-action 1

./bench -w 1 -d 5 iograph-lpm-bounded-prefilter \
	--prefixes /path/to/prefixes-100.txt \
	--selector /drop/event \
	--drop-action 1

./bench -w 1 -d 5 iograph-compact-acquire-decision \
	--blob /path/to/policy-1000.iog \
	--selector /drop/event \
	--probe-len 44

./bench -w 1 -d 5 iograph-lpm-bounded-acquire-decision \
	--prefixes /path/to/prefixes-1000.txt \
	--selector /drop/event \
	--probe-len 44

./bench -w 1 -d 5 iograph-discard-after-reserve \
	--blob /path/to/policy-1000.iog \
	--selector /drop/event \
	--drop-action 1

./bench -w 1 -d 5 iograph-compact-decision \
	--blob /path/to/policy-entries64.iog \
	--selector /drop/event \
	--entry-id 63

./bench -w 1 -d 5 iograph-compact-idx-decision \
	--blob /path/to/policy-entries64.iog \
	--selector /drop/event \
	--entry-idx 63

for size in 300 800 2048; do
	./bench -w 1 -d 5 -c 1 iograph-ringbuf-always-post \
		--payload-size "$size"

	./bench -w 1 -d 5 -c 1 iograph-compact-post-payload \
		--blob /path/to/policy-100.iog \
		--selector /no/match/post \
		--drop-action 2 \
		--payload-size "$size"
done
```

Repeat for 100 and 1000 typical policies. Add 1000 shared-prefix and long-path
rows when the LPM key length remains representable.

## Results

| dataset | prefixes | case | row | ops_M/s | ns/op | floor_delta_ns | ringbuf_on_reject | notes |
|:---|---:|:---|:---|---:|---:|---:|:---|:---|
| typical | 100 | floor | iograph-hook-floor | | | 0.0 | n/a | |
| typical | 100 | hit | iograph-compact-decision | | | | n/a | |
| typical | 100 | hit DROP | iograph-compact-prefilter | | | | 0 B | |
| typical | 100 | hit | iograph-lpm-decision | | | | n/a | |
| typical | 100 | hit | iograph-lpm-bounded-decision | | | | n/a | |
| typical | 1000 | floor | iograph-hook-floor | | | 0.0 | n/a | |
| typical | 1000 | hit | iograph-compact-decision | | | | n/a | |
| typical | 1000 | hit DROP | iograph-compact-prefilter | | | | 0 B | |
| typical | 1000 | hit | iograph-lpm-decision | | | | n/a | |
| typical | 1000 | hit | iograph-lpm-bounded-decision | | | | n/a | |
| typical | 1000 | hit | iograph-compact-acquire-decision | | | | n/a | |
| typical | 1000 | hit | iograph-lpm-bounded-acquire-decision | | | | n/a | |
| typical | 1000 | hit DROP | iograph-discard-after-reserve | | | | reserve/discard | |
| typical | 1000 | 64-entry hit | iograph-compact-decision --entry-id 63 | | | | n/a | |
| typical | 1000 | 64-entry hit | iograph-compact-idx-decision --entry-idx 63 | | | | n/a | |
| typical | 1000 | POST 300B | iograph-ringbuf-always-post | | | | post | |
| typical | 1000 | POST 300B | iograph-compact-post-payload | | | | post | |
| typical | 1000 | POST 800B | iograph-ringbuf-always-post | | | | post | |
| typical | 1000 | POST 800B | iograph-compact-post-payload | | | | post | |
| typical | 1000 | POST 2048B | iograph-ringbuf-always-post | | | | post | |
| typical | 1000 | POST 2048B | iograph-compact-post-payload | | | | post | |

## PMU Counters

Use `na` when the CPU PMU is not exposed by the host or VM.

| dataset | prefixes | row | cycles/op | branch_miss/op | l1d_miss/op | llc_miss/op | icache_miss/op |
|:---|---:|:---|---:|---:|---:|---:|---:|
| typical | 100 | iograph-compact-decision | | | | | |
| typical | 100 | iograph-lpm-decision | | | | | |
| typical | 1000 | iograph-compact-decision | | | | | |
| typical | 1000 | iograph-lpm-decision | | | | | |

## Reading

- Treat old byte-trie kernel rows as historical snapshots.
- Read compact rows against the same-hook floor.
- Record `compact_delta_ns = compact_decision_ns - floor_ns`.
- Record `lpm_delta_ns = lpm_decision_ns - floor_ns`.
- Do not paste userspace rows here as kernel measurements.
