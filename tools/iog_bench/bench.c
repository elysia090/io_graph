#include "bench_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#define HAVE_RDTSC 1
#else
#define HAVE_RDTSC 0
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SAMPLE_NR IOG_BENCH_SAMPLE_NR
#define QUANTILE_NR 1024u
#define BPF_RINGBUF_HDR_SZ_MODEL IOG_BENCH_RINGBUF_HDR_SZ_MODEL
#define BPF_RINGBUF_CAP_MODEL IOG_BENCH_RINGBUF_CAP_MODEL
#define CACHELINE_SZ_MODEL 64u

struct iog_run_state_size_probe {
	u32 state;
	u32 cursor;
	u32 action_code;
};

struct ringbuf_reserve_state {
	u64 mask;
	u64 consumer_pos;
	u64 producer_pos;
	u64 reserve_fail;
};

struct bench_result {
	double ns_op;
	double batch_p95_ns_op;
	double batch_p99_ns_op;
	double batch_p999_ns_op;
	double cycles_op;
	double branch_miss_op;
	double cache_miss_op;
	double l1d_miss_op;
	double llc_miss_op;
};

struct case_result {
	struct bench_result iog;
	struct bench_result compact;
	struct bench_result chain;
	struct bench_result list;
};

struct payload_result {
	u32 payload;
	struct bench_result materialize;
};

struct update_result {
	double verify_us;
	double compact_build_us;
	double update_us;
	double reclaim_us;
	u64 update_scratch_bytes;
	u64 peak_new_update_bytes;
	u64 peak_with_retired_bytes;
	u64 active_blob_bytes;
	u64 active_compact_bytes;
	u64 active_mem_bytes;
	u64 retired_mem_bytes;
	u64 total_mem_bytes;
	u64 update_seq;
	u32 retired_graphs;
	u32 reclaimed_graphs;
};

struct map_ops_result {
	u64 mem_usage_bytes;
	u64 action_only_mem_usage_bytes;
	u64 update_seq;
	long update_ret;
	long action_only_update_ret;
	u32 action_only_run_action;
	long action_only_run_ret;
	long delete_ret;
	long lookup_unsupported;
	long next_key_unsupported;
	u32 retired_graphs_after_delete;
	u32 reclaimed_graphs;
};

typedef u32 (*match_fn_t)(const struct run_ctx *ctx, const u8 *buf, u32 len);

static volatile u32 sink32;
static volatile u64 sink64;

static const u32 rates[] = { 100000, 500000, 1000000 };
static const double drops[] = { 0.90, 0.95, 0.97, 0.99 };
static const u32 payloads[] = { 300, 800, 2048 };

static struct event_record *ringbuf_event_at(u8 *base)
{
	return (struct event_record *)(void *)(base + BPF_RINGBUF_HDR_SZ_MODEL);
}

static void ringbuf_reserve_state_init(struct ringbuf_reserve_state *rb,
				       u32 cap)
{
	memset(rb, 0, sizeof(*rb));
	rb->mask = cap - 1u;
}

static u8 *ringbuf_reserve_state_reserve(struct ringbuf_reserve_state *rb,
					 u8 *area, size_t rec_bytes)
{
	u64 prod = rb->producer_pos;
	u64 new_prod = prod + rec_bytes;

	if (rec_bytes > rb->mask + 1u ||
	    new_prod - rb->consumer_pos > rb->mask) {
		rb->reserve_fail++;
		return NULL;
	}

	rb->producer_pos = new_prod;
	return area + (prod & rb->mask);
}

static void ringbuf_reserve_state_commit_and_drain(struct ringbuf_reserve_state *rb)
{
	rb->consumer_pos = rb->producer_pos;
}

static u64 cachelines_for_bytes(size_t bytes)
{
	return (bytes + CACHELINE_SZ_MODEL - 1u) / CACHELINE_SZ_MODEL;
}

static u64 nsec_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static u64 rdtsc_now(void)
{
#if HAVE_RDTSC
#if defined(_MSC_VER)
	return __rdtsc();
#else
	u32 lo, hi;

	__asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
	return ((u64)hi << 32) | lo;
#endif
#else
	return 0;
#endif
}

struct perf_sample {
	bool ok;
	u64 branch_misses;
	u64 cache_misses;
	u64 l1d_misses;
	u64 llc_misses;
};

struct perf_run {
	int branch_fd;
	int cache_fd;
	int l1d_fd;
	int llc_fd;
	int branch_err;
	int cache_err;
	int l1d_err;
	int llc_err;
	bool ok;
};

#ifdef __linux__
static int perf_open_counter(u32 type, u64 config)
{
	struct perf_event_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.type = type;
	attr.size = sizeof(attr);
	attr.config = config;
	attr.disabled = 1;
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;

	return (int)syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
}

static int perf_start(struct perf_run *run)
{
	memset(run, 0, sizeof(*run));
	run->branch_fd = -1;
	run->cache_fd = -1;
	run->l1d_fd = -1;
	run->llc_fd = -1;

	run->branch_fd = perf_open_counter(PERF_TYPE_HARDWARE,
					   PERF_COUNT_HW_BRANCH_MISSES);
	run->branch_err = run->branch_fd < 0 ? errno : 0;
	run->cache_fd = perf_open_counter(PERF_TYPE_HARDWARE,
					  PERF_COUNT_HW_CACHE_MISSES);
	run->cache_err = run->cache_fd < 0 ? errno : 0;
	run->l1d_fd = perf_open_counter(PERF_TYPE_HW_CACHE,
					PERF_COUNT_HW_CACHE_L1D |
					(PERF_COUNT_HW_CACHE_OP_READ << 8) |
					(PERF_COUNT_HW_CACHE_RESULT_MISS << 16));
	run->l1d_err = run->l1d_fd < 0 ? errno : 0;
	run->llc_fd = perf_open_counter(PERF_TYPE_HW_CACHE,
					PERF_COUNT_HW_CACHE_LL |
					(PERF_COUNT_HW_CACHE_OP_READ << 8) |
					(PERF_COUNT_HW_CACHE_RESULT_MISS << 16));
	run->llc_err = run->llc_fd < 0 ? errno : 0;

	run->ok = run->branch_fd >= 0 || run->cache_fd >= 0 ||
		  run->l1d_fd >= 0 || run->llc_fd >= 0;
	if (!run->ok)
		return -ENOTSUP;

	if (run->branch_fd >= 0) {
		ioctl(run->branch_fd, PERF_EVENT_IOC_RESET, 0);
		ioctl(run->branch_fd, PERF_EVENT_IOC_ENABLE, 0);
	}
	if (run->cache_fd >= 0) {
		ioctl(run->cache_fd, PERF_EVENT_IOC_RESET, 0);
		ioctl(run->cache_fd, PERF_EVENT_IOC_ENABLE, 0);
	}
	if (run->l1d_fd >= 0) {
		ioctl(run->l1d_fd, PERF_EVENT_IOC_RESET, 0);
		ioctl(run->l1d_fd, PERF_EVENT_IOC_ENABLE, 0);
	}
	if (run->llc_fd >= 0) {
		ioctl(run->llc_fd, PERF_EVENT_IOC_RESET, 0);
		ioctl(run->llc_fd, PERF_EVENT_IOC_ENABLE, 0);
	}
	return 0;
}

static void perf_stop(struct perf_run *run, struct perf_sample *sample)
{
	u64 value;

	memset(sample, 0, sizeof(*sample));
	if (!run->ok)
		goto out;

	if (run->branch_fd >= 0)
		ioctl(run->branch_fd, PERF_EVENT_IOC_DISABLE, 0);
	if (run->cache_fd >= 0)
		ioctl(run->cache_fd, PERF_EVENT_IOC_DISABLE, 0);
	if (run->l1d_fd >= 0)
		ioctl(run->l1d_fd, PERF_EVENT_IOC_DISABLE, 0);
	if (run->llc_fd >= 0)
		ioctl(run->llc_fd, PERF_EVENT_IOC_DISABLE, 0);

	if (run->branch_fd >= 0 &&
	    read(run->branch_fd, &value, sizeof(value)) == sizeof(value))
		sample->branch_misses = value;
	if (run->cache_fd >= 0 &&
	    read(run->cache_fd, &value, sizeof(value)) == sizeof(value))
		sample->cache_misses = value;
	if (run->l1d_fd >= 0 &&
	    read(run->l1d_fd, &value, sizeof(value)) == sizeof(value))
		sample->l1d_misses = value;
	if (run->llc_fd >= 0 &&
	    read(run->llc_fd, &value, sizeof(value)) == sizeof(value))
		sample->llc_misses = value;
	sample->ok = true;

out:
	if (run->branch_fd >= 0)
		close(run->branch_fd);
	if (run->cache_fd >= 0)
		close(run->cache_fd);
	if (run->l1d_fd >= 0)
		close(run->l1d_fd);
	if (run->llc_fd >= 0)
		close(run->llc_fd);
}

static void perf_describe(char *buf, size_t len)
{
	struct perf_run run;
	struct perf_sample sample;

	if (!buf || !len)
		return;

	if (!perf_start(&run)) {
		snprintf(buf, len, "available");
	} else {
		snprintf(buf, len,
			 "unavailable(branch=%s cache=%s l1d=%s llc=%s)",
			 strerror(run.branch_err ? run.branch_err : ENOTSUP),
			 strerror(run.cache_err ? run.cache_err : ENOTSUP),
			 strerror(run.l1d_err ? run.l1d_err : ENOTSUP),
			 strerror(run.llc_err ? run.llc_err : ENOTSUP));
	}
	perf_stop(&run, &sample);
}
#else
static int perf_start(struct perf_run *run)
{
	memset(run, 0, sizeof(*run));
	run->branch_fd = -1;
	run->cache_fd = -1;
	run->l1d_fd = -1;
	run->llc_fd = -1;
	return -ENOTSUP;
}

static void perf_stop(struct perf_run *run, struct perf_sample *sample)
{
	(void)run;
	memset(sample, 0, sizeof(*sample));
}

static void perf_describe(char *buf, size_t len)
{
	snprintf(buf, len, "unavailable(non-linux)");
}
#endif

static char *xstrdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *out = malloc(len);

	if (!out) {
		perror("malloc");
		exit(2);
	}
	memcpy(out, s, len);
	return out;
}

static int compare_double(const void *left, const void *right)
{
	double a = *(const double *)left;
	double b = *(const double *)right;

	return (a > b) - (a < b);
}

static double sorted_quantile(const double *values, size_t nr, u32 permille)
{
	size_t idx;

	if (!nr)
		return -1.0;

	idx = ((size_t)permille * nr + 999u) / 1000u;
	if (!idx)
		return values[0];
	if (idx > nr)
		idx = nr;
	return values[idx - 1u];
}

static struct bench_result bench_match(const struct run_ctx *ctx,
				       const struct sample *samples,
				       size_t sample_nr, match_fn_t fn,
				       u64 iterations)
{
	struct perf_run perf;
	struct perf_sample perf_sample;
	struct bench_result res;
	double batch_ns[QUANTILE_NR];
	u64 start_ns, end_ns, start_cycles, end_cycles;
	u64 i, done = 0;
	u32 local = 0;
	size_t batch_nr, batch;
	int perf_ret;

	for (i = 0; i < 32768; i++) {
		const struct sample *s = &samples[i & (sample_nr - 1)];

		local ^= fn(ctx, s->bytes, s->len);
	}
	sink32 = local;

	perf_ret = perf_start(&perf);
	start_cycles = rdtsc_now();
	start_ns = nsec_now();

	batch_nr = iterations < QUANTILE_NR ? (size_t)iterations : QUANTILE_NR;
	for (batch = 0; batch < batch_nr; batch++) {
		u64 batch_iters = iterations / batch_nr;
		u64 batch_start;

		if (batch < iterations % batch_nr)
			batch_iters++;
		batch_start = nsec_now();
		for (i = 0; i < batch_iters; i++) {
			const struct sample *s =
				&samples[(done + i) & (sample_nr - 1)];

			local ^= fn(ctx, s->bytes, s->len);
		}
		batch_ns[batch] = (double)(nsec_now() - batch_start) /
				  (double)batch_iters;
		done += batch_iters;
	}

	end_ns = nsec_now();
	end_cycles = rdtsc_now();
	perf_stop(&perf, &perf_sample);
	sink32 = local;

	memset(&res, 0, sizeof(res));
	res.ns_op = (double)(end_ns - start_ns) / (double)iterations;
	qsort(batch_ns, batch_nr, sizeof(batch_ns[0]), compare_double);
	res.batch_p95_ns_op = sorted_quantile(batch_ns, batch_nr, 950);
	res.batch_p99_ns_op = sorted_quantile(batch_ns, batch_nr, 990);
	res.batch_p999_ns_op = sorted_quantile(batch_ns, batch_nr, 999);
	if (HAVE_RDTSC && end_cycles > start_cycles)
		res.cycles_op = (double)(end_cycles - start_cycles) /
				(double)iterations;
	else
		res.cycles_op = -1.0;

	res.branch_miss_op = (!perf_ret && perf.branch_fd >= 0) ?
		(double)perf_sample.branch_misses / (double)iterations : -1.0;
	res.cache_miss_op = (!perf_ret && perf.cache_fd >= 0) ?
		(double)perf_sample.cache_misses / (double)iterations : -1.0;
	res.l1d_miss_op = (!perf_ret && perf.l1d_fd >= 0) ?
		(double)perf_sample.l1d_misses / (double)iterations : -1.0;
	res.llc_miss_op = (!perf_ret && perf.llc_fd >= 0) ?
		(double)perf_sample.llc_misses / (double)iterations : -1.0;

	return res;
}

static void fill_payload(u8 *src, u32 payload)
{
	u32 i;

	for (i = 0; i < payload; i++)
		src[i] = (u8)(0x20u + (i % 87u));
}

static void decode_event(const struct event_record *rec,
			 struct decoded_event *event)
{
	const u8 *payload = rec->payload;
	u32 half = rec->payload_len / 2;

	event->path = payload + rec->selector_off;
	event->path_len = rec->selector_len;
	event->cmdline = payload + half / 2;
	event->cmdline_len = half;
	event->arg = payload + half;
	event->arg_len = rec->payload_len - half;
	event->event_type = rec->event_type;
	event->pid_tgid = rec->pid_tgid;
	event->scratch[0] = rec->ts_ns;
	event->scratch[1] = rec->payload_len;
	event->scratch[2] = rec->selector_len;
}

static struct bench_result bench_materialize(u32 payload, u64 iterations)
{
	struct bench_result res;
	u8 *src, *dst;
	size_t rec_bytes = ringbuf_record_bytes(payload);
	size_t cap = rec_bytes * 4096u;
	u64 start_ns, end_ns, start_cycles, end_cycles;
	u64 i;
	u64 local = 0;

	src = malloc(payload);
	dst = malloc(cap);
	if (!src || !dst) {
		perror("malloc");
		exit(2);
	}
	fill_payload(src, payload);
	memset(dst, 0, cap);

	for (i = 0; i < 32768; i++) {
		struct event_record *rec;
		struct decoded_event event;
		size_t off = (size_t)(i % 4096u) * rec_bytes;

		rec = ringbuf_event_at(dst + off);
		rec->ts_ns = i;
		rec->pid_tgid = 0x12340000ull + i;
		rec->event_type = 7;
		rec->payload_len = payload;
		rec->selector_off = 0;
		rec->selector_len = payload < 64 ? payload : 64;
		memcpy(rec->payload, src, payload);
		decode_event(rec, &event);
		local += event.path_len + event.arg_len + event.scratch[0];
	}
	sink64 = local;

	start_cycles = rdtsc_now();
	start_ns = nsec_now();
	for (i = 0; i < iterations; i++) {
		struct event_record *rec;
		struct decoded_event event;
		size_t off = (size_t)(i % 4096u) * rec_bytes;

		rec = ringbuf_event_at(dst + off);
		rec->ts_ns = i;
		rec->pid_tgid = 0x12340000ull + i;
		rec->event_type = 7;
		rec->payload_len = payload;
		rec->selector_off = 0;
		rec->selector_len = payload < 64 ? payload : 64;
		memcpy(rec->payload, src, payload);
		decode_event(rec, &event);
		local += event.path[0] + event.cmdline_len + event.scratch[2];
	}
	end_ns = nsec_now();
	end_cycles = rdtsc_now();
	sink64 = local;

	memset(&res, 0, sizeof(res));
	res.ns_op = (double)(end_ns - start_ns) / (double)iterations;
	if (HAVE_RDTSC && end_cycles > start_cycles)
		res.cycles_op = (double)(end_cycles - start_cycles) /
				(double)iterations;
	else
		res.cycles_op = -1.0;
	res.branch_miss_op = -1.0;
	res.cache_miss_op = -1.0;
	res.l1d_miss_op = -1.0;
	res.llc_miss_op = -1.0;

	free(dst);
	free(src);
	return res;
}

static struct bench_result
bench_bpf_event_path(const struct run_ctx *ctx, const struct sample *samples,
		     size_t sample_nr, u32 payload, u64 iterations,
		     double *emitted_ringbuf_b_op, double *reserve_fail_op)
{
	struct perf_run perf;
	struct perf_sample perf_sample;
	struct bench_result res;
	struct ringbuf_reserve_state rb;
	u8 *src, *ring;
	size_t rec_bytes = ringbuf_record_bytes(payload);
	size_t cap = BPF_RINGBUF_CAP_MODEL;
	size_t backing_cap = cap + rec_bytes;
	u64 start_ns, end_ns, start_cycles, end_cycles;
	u64 emitted_bytes = 0;
	u64 i;
	u64 local = 0;
	int perf_ret;

	src = malloc(payload);
	ring = malloc(backing_cap);
	if (!src || !ring) {
		perror("malloc");
		exit(2);
	}
	fill_payload(src, payload);
	memset(ring, 0, backing_cap);
	ringbuf_reserve_state_init(&rb, BPF_RINGBUF_CAP_MODEL);

	for (i = 0; i < 32768; i++) {
		const struct sample *s = &samples[i & (sample_nr - 1)];
		u32 action = iog_bpf_kfunc_run_action(ctx->bpf_map, s->bytes,
						      s->len, 0);

		if (action) {
			u8 *slot = ringbuf_reserve_state_reserve(&rb, ring,
								 rec_bytes);
			struct event_record *rec;

			if (!slot) {
				local += action;
				continue;
			}

			rec = ringbuf_event_at(slot);
			rec->ts_ns = i;
			rec->pid_tgid = 0x12340000ull + i;
			rec->event_type = action;
			rec->payload_len = payload;
			rec->selector_off = 0;
			rec->selector_len = s->len < payload ? s->len : payload;
			memcpy(rec->payload, src, payload);
			local += rec->payload_len + rec->selector_len +
				 rec->event_type;
			ringbuf_reserve_state_commit_and_drain(&rb);
		} else {
			local += s->len;
		}
	}
	sink64 = local;

	perf_ret = perf_start(&perf);
	start_cycles = rdtsc_now();
	start_ns = nsec_now();

	for (i = 0; i < iterations; i++) {
		const struct sample *s = &samples[i & (sample_nr - 1)];
		u32 action = iog_bpf_kfunc_run_action(ctx->bpf_map, s->bytes,
						      s->len, 0);

		if (action) {
			u8 *slot = ringbuf_reserve_state_reserve(&rb, ring,
								 rec_bytes);
			struct event_record *rec;

			if (!slot) {
				local += action;
				continue;
			}

			rec = ringbuf_event_at(slot);
			rec->ts_ns = i;
			rec->pid_tgid = 0x12340000ull + i;
			rec->event_type = action;
			rec->payload_len = payload;
			rec->selector_off = 0;
			rec->selector_len = s->len < payload ? s->len : payload;
			memcpy(rec->payload, src, payload);
			emitted_bytes += rec_bytes;
			local += rec->payload[0] + rec->selector_len +
				 rec->event_type;
			ringbuf_reserve_state_commit_and_drain(&rb);
		} else {
			local += s->len;
		}
	}

	end_ns = nsec_now();
	end_cycles = rdtsc_now();
	perf_stop(&perf, &perf_sample);
	sink64 = local;

	memset(&res, 0, sizeof(res));
	res.ns_op = (double)(end_ns - start_ns) / (double)iterations;
	if (HAVE_RDTSC && end_cycles > start_cycles)
		res.cycles_op = (double)(end_cycles - start_cycles) /
				(double)iterations;
	else
		res.cycles_op = -1.0;
	res.branch_miss_op = (!perf_ret && perf.branch_fd >= 0) ?
		(double)perf_sample.branch_misses / (double)iterations : -1.0;
	res.cache_miss_op = (!perf_ret && perf.cache_fd >= 0) ?
		(double)perf_sample.cache_misses / (double)iterations : -1.0;
	res.l1d_miss_op = (!perf_ret && perf.l1d_fd >= 0) ?
		(double)perf_sample.l1d_misses / (double)iterations : -1.0;
	res.llc_miss_op = (!perf_ret && perf.llc_fd >= 0) ?
		(double)perf_sample.llc_misses / (double)iterations : -1.0;

	if (emitted_ringbuf_b_op)
		*emitted_ringbuf_b_op = (double)emitted_bytes /
					(double)iterations;
	if (reserve_fail_op)
		*reserve_fail_op = (double)rb.reserve_fail /
				   (double)(iterations + 32768u);

	free(ring);
	free(src);
	return res;
}

static struct bench_result
bench_selector_acquire_decision(const struct run_ctx *ctx,
				const struct sample *samples, size_t sample_nr,
				u32 probe_len, bool bounded_probe,
				u64 iterations, double *copied_b_op)
{
	struct bench_result res;
	u8 buf[sizeof(samples[0].bytes)];
	u64 start_ns, end_ns, start_cycles, end_cycles;
	u64 copied = 0;
	u64 i;
	u32 local = 0;

	for (i = 0; i < 32768; i++) {
		const struct sample *s = &samples[i & (sample_nr - 1)];
		u32 len = s->len;

		if (bounded_probe && len > probe_len)
			len = probe_len;
		if (len > sizeof(buf))
			len = sizeof(buf);
		memcpy(buf, s->bytes, len);
		local ^= iog_bpf_kfunc_run_action(ctx->bpf_map, buf, len, 0);
	}
	sink32 = local;

	start_cycles = rdtsc_now();
	start_ns = nsec_now();
	for (i = 0; i < iterations; i++) {
		const struct sample *s = &samples[i & (sample_nr - 1)];
		u32 len = s->len;

		if (bounded_probe && len > probe_len)
			len = probe_len;
		if (len > sizeof(buf))
			len = sizeof(buf);
		memcpy(buf, s->bytes, len);
		copied += len;
		local ^= iog_bpf_kfunc_run_action(ctx->bpf_map, buf, len, 0);
	}
	end_ns = nsec_now();
	end_cycles = rdtsc_now();
	sink32 = local;

	memset(&res, 0, sizeof(res));
	res.ns_op = (double)(end_ns - start_ns) / (double)iterations;
	if (HAVE_RDTSC && end_cycles > start_cycles)
		res.cycles_op = (double)(end_cycles - start_cycles) /
				(double)iterations;
	else
		res.cycles_op = -1.0;
	res.branch_miss_op = -1.0;
	res.cache_miss_op = -1.0;
	res.l1d_miss_op = -1.0;
	res.llc_miss_op = -1.0;
	if (copied_b_op)
		*copied_b_op = (double)copied / (double)iterations;
	return res;
}

static struct bench_result
bench_discard_after_reserve(const struct run_ctx *ctx,
			    const struct sample *samples, size_t sample_nr,
			    u32 payload, u64 iterations,
			    double *reserved_ringbuf_b_op,
			    double *emitted_ringbuf_b_op,
			    double *reserve_fail_op)
{
	struct bench_result res;
	struct ringbuf_reserve_state rb;
	u8 *src, *ring;
	size_t rec_bytes = ringbuf_record_bytes(payload);
	size_t cap = BPF_RINGBUF_CAP_MODEL;
	size_t backing_cap = cap + rec_bytes;
	u64 start_ns, end_ns, start_cycles, end_cycles;
	u64 reserved_bytes = 0, emitted_bytes = 0;
	u64 i;
	u64 local = 0;

	src = malloc(payload);
	ring = malloc(backing_cap);
	if (!src || !ring) {
		perror("malloc");
		exit(2);
	}
	fill_payload(src, payload);
	memset(ring, 0, backing_cap);
	ringbuf_reserve_state_init(&rb, BPF_RINGBUF_CAP_MODEL);

	for (i = 0; i < 32768; i++) {
		const struct sample *s = &samples[i & (sample_nr - 1)];
		u8 *slot = ringbuf_reserve_state_reserve(&rb, ring, rec_bytes);
		struct event_record *rec;
		u32 action;

		if (!slot)
			continue;
		rec = ringbuf_event_at(slot);
		rec->ts_ns = i;
		rec->pid_tgid = 0x12340000ull + i;
		rec->event_type = 0;
		rec->payload_len = payload;
		rec->selector_off = 0;
		rec->selector_len = s->len < payload ? s->len : payload;
		memcpy(rec->payload, src, payload);
		action = iog_bpf_kfunc_run_action(ctx->bpf_map, s->bytes,
						  s->len, 0);
		local += action + rec->selector_len;
		ringbuf_reserve_state_commit_and_drain(&rb);
	}
	sink64 = local;

	start_cycles = rdtsc_now();
	start_ns = nsec_now();
	for (i = 0; i < iterations; i++) {
		const struct sample *s = &samples[i & (sample_nr - 1)];
		u8 *slot = ringbuf_reserve_state_reserve(&rb, ring, rec_bytes);
		struct event_record *rec;
		u32 action;

		if (!slot) {
			local += s->len;
			continue;
		}
		rec = ringbuf_event_at(slot);
		rec->ts_ns = i;
		rec->pid_tgid = 0x12340000ull + i;
		rec->event_type = 0;
		rec->payload_len = payload;
		rec->selector_off = 0;
		rec->selector_len = s->len < payload ? s->len : payload;
		memcpy(rec->payload, src, payload);
		reserved_bytes += rec_bytes;
		action = iog_bpf_kfunc_run_action(ctx->bpf_map, s->bytes,
						  s->len, 0);
		if (action) {
			rec->event_type = action;
			emitted_bytes += rec_bytes;
		}
		local += rec->payload[0] + rec->selector_len + action;
		ringbuf_reserve_state_commit_and_drain(&rb);
	}
	end_ns = nsec_now();
	end_cycles = rdtsc_now();
	sink64 = local;

	memset(&res, 0, sizeof(res));
	res.ns_op = (double)(end_ns - start_ns) / (double)iterations;
	if (HAVE_RDTSC && end_cycles > start_cycles)
		res.cycles_op = (double)(end_cycles - start_cycles) /
				(double)iterations;
	else
		res.cycles_op = -1.0;
	res.branch_miss_op = -1.0;
	res.cache_miss_op = -1.0;
	res.l1d_miss_op = -1.0;
	res.llc_miss_op = -1.0;
	if (reserved_ringbuf_b_op)
		*reserved_ringbuf_b_op = (double)reserved_bytes /
					 (double)iterations;
	if (emitted_ringbuf_b_op)
		*emitted_ringbuf_b_op = (double)emitted_bytes /
					(double)iterations;
	if (reserve_fail_op)
		*reserve_fail_op = (double)rb.reserve_fail /
				   (double)(iterations + 32768u);

	free(ring);
	free(src);
	return res;
}

static void print_metric(double value)
{
	if (value < 0.0)
		printf("na");
	else
		printf("%.2f", value);
}

static void print_decision_row(size_t prefixes, const char *case_name,
			       const char *matcher,
			       const struct bench_result *res)
{
	printf("| %zu | %s | %s | %.2f | ", prefixes, case_name, matcher,
	       res->ns_op);
	printf("%.2f | %.2f | %.2f | ", res->batch_p95_ns_op,
	       res->batch_p99_ns_op, res->batch_p999_ns_op);
	print_metric(res->cycles_op);
	printf(" | ");
	print_metric(res->branch_miss_op);
	printf(" | ");
	print_metric(res->l1d_miss_op);
	printf(" | ");
	print_metric(res->llc_miss_op);
	printf(" | 0 |\n");
}

static double sample_mean_len(const struct sample *samples, size_t nr)
{
	u64 bytes = 0;
	size_t i;

	for (i = 0; i < nr; i++)
		bytes += samples[i].len;

	return nr ? (double)bytes / (double)nr : 0.0;
}

static double sample_mean_compact_transitions(const struct iog_cgraph *cg,
					      const struct sample *samples,
					      size_t nr)
{
	u64 transitions = 0;
	size_t i;

	for (i = 0; i < nr; i++)
		transitions += iog_cgraph_count_transitions(cg, samples[i].bytes,
							    samples[i].len);

	return nr ? (double)transitions / (double)nr : 0.0;
}

static void print_matched_path_row(size_t prefixes, const char *case_name,
				   const char *matcher,
				   const struct bench_result *res,
				   double input_bytes, double transitions)
{
	printf("| %zu | %s | %s | %.2f | %.2f | %.2f | %.3f | %.3f | ",
	       prefixes, case_name, matcher, input_bytes, transitions,
	       res->ns_op, input_bytes ? res->ns_op / input_bytes : 0.0,
	       transitions ? res->ns_op / transitions : 0.0);
	printf("%.2f | %.2f | %.2f | ", res->batch_p95_ns_op,
	       res->batch_p99_ns_op, res->batch_p999_ns_op);
	print_metric(res->cycles_op);
	printf(" | ");
	print_metric(res->branch_miss_op);
	printf(" | ");
	print_metric(res->l1d_miss_op);
	printf(" | ");
	print_metric(res->llc_miss_op);
	printf(" |\n");
}

static void print_bpf_event_path_row(size_t prefixes, const char *case_name,
				     u32 payload,
				     const struct bench_result *res,
				     double emitted_ringbuf_b_op,
				     double reserve_fail_op)
{
	printf("| %zu | %s | %u | %.2f | ", prefixes, case_name, payload,
	       res->ns_op);
	print_metric(res->cycles_op);
	printf(" | ");
	print_metric(res->branch_miss_op);
	printf(" | ");
	print_metric(res->l1d_miss_op);
	printf(" | ");
	print_metric(res->llc_miss_op);
	printf(" | %.2f | %.6f | 0 |\n", emitted_ringbuf_b_op,
	       reserve_fail_op);
}

static void print_selector_acquisition_row(size_t prefixes,
					   const char *case_name,
					   const char *mode, u32 probe_len,
					   const struct bench_result *res,
					   double copied_b_op)
{
	printf("| %zu | %s | %s | %u | %.2f | %.2f | ", prefixes,
	       case_name, mode, probe_len, copied_b_op, res->ns_op);
	print_metric(res->cycles_op);
	printf(" | 0 |\n");
}

static void print_drop_order_row(size_t prefixes, const char *case_name,
				 u32 payload, const char *order,
				 const struct bench_result *res,
				 double reserved_b_op, double emitted_b_op,
				 double reserve_fail_op)
{
	printf("| %zu | %s | %u | %s | %.2f | ", prefixes, case_name,
	       payload, order, res->ns_op);
	print_metric(res->cycles_op);
	printf(" | %.2f | %.2f | %.6f | 0 |\n", reserved_b_op, emitted_b_op,
	       reserve_fail_op);
}

static void print_io_accounting_rows(void)
{
	size_t i;

	printf("\nio-aware object accounting\n");
	printf("| payload_B | event_record_B | bpf_ringbuf_record_B | ringbuf_cachelines | decoded_event_B | postdrop_intermediate_B | iog_run_state_B | drop_path_ringbuf_B | avoided_dirty_cachelines_per_1M_95pct_drop |\n");
	printf("|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
	for (i = 0; i < ARRAY_SIZE(payloads); i++) {
		u32 payload = payloads[i];
		size_t record = ringbuf_record_bytes(payload);
		u64 cachelines = cachelines_for_bytes(record);

		printf("| %u | %zu | %zu | %" PRIu64 " | %zu | %zu | %zu | 0 | %.2fM/s |\n",
		       payload, event_record_bytes(payload),
		       record, cachelines,
		       sizeof(struct decoded_event),
		       postdrop_intermediate_bytes(payload),
		       sizeof(struct iog_run_state_size_probe),
		       (double)cachelines * 950000.0 / 1000000.0);
	}
}

static struct update_result bench_update(const void *blob, size_t blob_len,
					 u64 iterations)
{
	struct update_result result;
	struct iog_cgraph *compact = NULL;
	struct iog_graph graph;
	struct iog_map map;
	u64 start_ns, end_ns, reclaim_start_ns, reclaim_end_ns;
	void *copy;
	u64 i;
	char err[256];
	int ret;

	memset(&result, 0, sizeof(result));
	memset(err, 0, sizeof(err));

	start_ns = nsec_now();
	ret = iog_verify_blob(blob, blob_len, &iog_default_limits, err,
			      sizeof(err));
	end_ns = nsec_now();
	if (ret) {
		fprintf(stderr, "verify failed in update bench: %s\n", err);
		exit(1);
	}
	result.verify_us = (double)(end_ns - start_ns) / 1000.0;

	copy = malloc(blob_len);
	if (!copy) {
		perror("malloc");
		exit(2);
	}
	memcpy(copy, blob, blob_len);
	ret = iog_graph_from_blob(&graph, copy, blob_len, &iog_default_limits,
				  err, sizeof(err));
	if (ret) {
		fprintf(stderr, "graph load failed in update bench: %s\n", err);
		exit(1);
	}
	ret = iog_graph_inline_accept_codes(&graph);
	if (ret) {
		fprintf(stderr, "accept inline failed in update bench: %d\n",
			ret);
		exit(1);
	}
	result.update_scratch_bytes =
		(u64)graph.hdr->node_cnt *
		(sizeof(bool) + 2u * sizeof(u32));
	start_ns = nsec_now();
	ret = iog_cgraph_new(&graph, &compact);
	end_ns = nsec_now();
	if (ret) {
		fprintf(stderr, "compact build failed in update bench: %d\n",
			ret);
		exit(1);
	}
	result.compact_build_us = (double)(end_ns - start_ns) / 1000.0;
	iog_cgraph_free(compact);
	free(copy);

	iog_map_init(&map);
	start_ns = nsec_now();
	for (i = 0; i < iterations; i++) {
		memset(err, 0, sizeof(err));
		ret = iog_map_update_blob(&map, blob, blob_len,
					  &iog_default_limits, err, sizeof(err));
		if (ret) {
			fprintf(stderr, "map update failed: %s\n", err);
			exit(1);
		}
	}
	end_ns = nsec_now();

	result.update_us = ((double)(end_ns - start_ns) / 1000.0) /
			   (double)iterations;
	if (map.graph) {
		result.active_blob_bytes = map.graph->blob_len;
		result.active_compact_bytes =
			iog_cgraph_mem_bytes(map.graph->compact);
	}
	result.active_mem_bytes = iog_map_active_mem_usage(&map);
	result.retired_mem_bytes = iog_map_retired_mem_usage(&map);
	result.total_mem_bytes = iog_map_mem_usage(&map);
	result.update_seq = map.update_seq;
	result.retired_graphs = map.retired_cnt;
	result.peak_new_update_bytes = result.active_mem_bytes +
				       result.update_scratch_bytes;
	result.peak_with_retired_bytes = result.total_mem_bytes +
					 result.update_scratch_bytes;

	reclaim_start_ns = nsec_now();
	result.reclaimed_graphs = iog_map_reclaim(&map);
	reclaim_end_ns = nsec_now();
	result.reclaim_us = (double)(reclaim_end_ns - reclaim_start_ns) /
			    1000.0;

	iog_map_destroy(&map);
	return result;
}

static struct map_ops_result exercise_bpf_map_ops(const void *blob,
						  size_t blob_len)
{
	struct map_ops_result result;
	struct iog_bpf_map *map = NULL;
	struct iog_bpf_map *action_only = NULL;
	struct iog_bpf_attr attr = {
		.key_size = IOG_BPF_KEY_SIZE,
		.value_size = (u32)blob_len,
		.max_entries = IOG_BPF_MAX_ENTRIES,
		.map_flags = 0,
	};
	u32 key = 0, next_key = 0;
	char err[256] = "";
	int ret;

	memset(&result, 0, sizeof(result));
	ret = iog_bpf_map_ops.map_alloc(&attr, &map);
	if (ret) {
		fprintf(stderr, "bpf map alloc failed: %d\n", ret);
		exit(1);
	}

	result.update_ret = iog_bpf_map_ops.map_update_elem(map, &key, blob,
							    IOG_BPF_ANY, err,
							    sizeof(err));
	if (result.update_ret) {
		fprintf(stderr, "bpf map update failed: %s\n", err);
		exit(1);
	}

	errno = 0;
	(void)iog_bpf_map_ops.map_lookup_elem(map, &key);
	result.lookup_unsupported = errno == ENOTSUP ? -ENOTSUP : -errno;
	result.next_key_unsupported =
		iog_bpf_map_ops.map_get_next_key(map, NULL, &next_key);
	result.mem_usage_bytes = iog_bpf_map_ops.map_mem_usage(map);
	result.update_seq = map->map.update_seq;
	result.delete_ret = iog_bpf_map_ops.map_delete_elem(map, &key);
	result.retired_graphs_after_delete = map->map.retired_cnt;
	result.reclaimed_graphs = iog_map_reclaim(&map->map);

	iog_bpf_map_ops.map_free(map);

	attr.map_flags = IOG_BPF_F_ACTION_ONLY;
	ret = iog_bpf_map_ops.map_alloc(&attr, &action_only);
	if (ret) {
		fprintf(stderr, "action-only bpf map alloc failed: %d\n", ret);
		exit(1);
	}
	result.action_only_update_ret =
		iog_bpf_map_ops.map_update_elem(action_only, &key, blob,
						IOG_BPF_ANY, err,
						sizeof(err));
	if (result.action_only_update_ret) {
		fprintf(stderr, "action-only bpf map update failed: %s\n", err);
		exit(1);
	}
	result.action_only_mem_usage_bytes =
		iog_bpf_map_ops.map_mem_usage(action_only);
	result.action_only_run_action =
		iog_bpf_kfunc_run_action(action_only,
					 (const u8 *)"/no/such/prefix", 15, 0);
	result.action_only_run_ret =
		iog_bpf_kfunc_run(action_only, (const u8 *)"/no/such/prefix",
				  15, 0, &(struct iog_run_result){ 0 });
	iog_bpf_map_ops.map_free(action_only);
	return result;
}

typedef bool (*verifier_mutator_t)(void *blob, size_t len);

static bool mutate_bad_magic(void *blob, size_t len)
{
	struct iog_blob_hdr *hdr = blob;

	if (len < sizeof(*hdr))
		return false;
	hdr->magic ^= 1u;
	return true;
}

static bool mutate_bad_section_order(void *blob, size_t len)
{
	struct iog_blob_hdr *hdr = blob;

	if (len < sizeof(*hdr))
		return false;
	hdr->edges_off += 4u;
	return true;
}

static bool mutate_bad_node_edges(void *blob, size_t len)
{
	struct iog_blob_hdr *hdr = blob;
	struct iog_node *nodes;

	if (len < sizeof(*hdr) || !hdr->node_cnt)
		return false;
	nodes = (void *)((u8 *)blob + hdr->nodes_off);
	nodes[0].edge_start = hdr->edge_cnt + 1u;
	nodes[0].edge_cnt = 1u;
	return true;
}

static bool mutate_bad_edge_dst(void *blob, size_t len)
{
	struct iog_blob_hdr *hdr = blob;
	struct iog_edge *edges;

	if (len < sizeof(*hdr) || !hdr->edge_cnt)
		return false;
	edges = (void *)((u8 *)blob + hdr->edges_off);
	edges[0].dst = hdr->node_cnt;
	return true;
}

static bool mutate_bad_accept_id(void *blob, size_t len)
{
	struct iog_blob_hdr *hdr = blob;
	struct iog_node *nodes;

	if (len < sizeof(*hdr) || !hdr->node_cnt || !hdr->accept_cnt)
		return false;
	nodes = (void *)((u8 *)blob + hdr->nodes_off);
	nodes[0].accept_id = hdr->accept_cnt;
	return true;
}

static bool mutate_unknown_node_flags(void *blob, size_t len)
{
	struct iog_blob_hdr *hdr = blob;
	struct iog_node *nodes;

	if (len < sizeof(*hdr) || !hdr->node_cnt)
		return false;
	nodes = (void *)((u8 *)blob + hdr->nodes_off);
	nodes[0].flags = 0x8000u;
	return true;
}

static bool mutate_unsorted_edges(void *blob, size_t len)
{
	struct iog_blob_hdr *hdr = blob;
	struct iog_node *nodes;
	struct iog_edge *edges;
	u32 i;

	if (len < sizeof(*hdr))
		return false;
	nodes = (void *)((u8 *)blob + hdr->nodes_off);
	edges = (void *)((u8 *)blob + hdr->edges_off);

	for (i = 0; i < hdr->node_cnt; i++) {
		struct iog_node *node = &nodes[i];
		struct iog_edge *a, *b;

		if (node->edge_cnt < 2)
			continue;

		a = &edges[node->edge_start];
		b = &edges[node->edge_start + 1u];
		b->sym_lo = a->sym_lo;
		b->sym_hi = a->sym_hi;
		return true;
	}

	return false;
}

static bool mutate_too_long_input(void *blob, size_t len)
{
	struct iog_blob_hdr *hdr = blob;

	if (len < sizeof(*hdr))
		return false;
	hdr->max_input_len = iog_default_limits.max_input_len + 1u;
	return true;
}

static u32 run_verifier_selftests(const void *blob, size_t blob_len)
{
	static const struct {
		const char *name;
		verifier_mutator_t mutate;
	} cases[] = {
		{ "bad_magic", mutate_bad_magic },
		{ "bad_section_order", mutate_bad_section_order },
		{ "bad_node_edges", mutate_bad_node_edges },
		{ "bad_edge_dst", mutate_bad_edge_dst },
		{ "bad_accept_id", mutate_bad_accept_id },
		{ "unknown_node_flags", mutate_unknown_node_flags },
		{ "unsorted_edges", mutate_unsorted_edges },
		{ "too_long_input", mutate_too_long_input },
	};
	u32 passed = 0;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		char err[256] = "";
		void *copy;
		int ret;

		copy = malloc(blob_len);
		if (!copy) {
			perror("malloc");
			exit(2);
		}
		memcpy(copy, blob, blob_len);

		if (!cases[i].mutate(copy, blob_len)) {
			fprintf(stderr, "verifier selftest did not mutate: %s\n",
				cases[i].name);
			free(copy);
			exit(1);
		}

		ret = iog_verify_blob(copy, blob_len, &iog_default_limits,
				      err, sizeof(err));
		if (!ret) {
			fprintf(stderr, "verifier selftest passed unexpectedly: %s\n",
				cases[i].name);
			free(copy);
			exit(1);
		}

		free(copy);
		passed++;
	}

	return passed;
}

static void assert_equiv(const struct run_ctx *ctx, const struct sample *samples,
			 size_t sample_nr)
{
	size_t i;

	for (i = 0; i < sample_nr; i++) {
		u32 a = match_iog(ctx, samples[i].bytes, samples[i].len);
		u32 b = match_iog_compact(ctx, samples[i].bytes,
					   samples[i].len);
		u32 c = match_chain(ctx, samples[i].bytes, samples[i].len);
		u32 d = match_list(ctx, samples[i].bytes, samples[i].len);

		if (a != b || a != c || a != d) {
			fprintf(stderr,
				"matcher mismatch at sample %zu: iog=%u compact=%u chain=%u list=%u\n",
				i, a, b, c, d);
			exit(1);
		}
	}
}

static double weighted_ns(const struct bench_result *neg,
			  const struct bench_result *hit, double drop)
{
	return drop * neg->ns_op + (1.0 - drop) * hit->ns_op;
}

static void print_estimate_rows(size_t prefixes, const char *neg_case,
				const struct case_result *neg,
				const struct case_result *hit,
				const struct payload_result *copies,
				size_t copy_nr)
{
	size_t r, d, p;

	for (r = 0; r < ARRAY_SIZE(rates); r++) {
		for (d = 0; d < ARRAY_SIZE(drops); d++) {
			double drop = drops[d];
			double post = 1.0 - drop;

			for (p = 0; p < copy_nr; p++) {
				double materialize_ns =
					copies[p].materialize.ns_op;
				double iog_decision = weighted_ns(&neg->iog,
								  &hit->iog,
								  drop);
				double compact_decision =
					weighted_ns(&neg->compact,
						    &hit->compact, drop);
				double chain_decision = weighted_ns(&neg->chain,
								    &hit->chain,
								    drop);
				double list_decision = weighted_ns(&neg->list,
								   &hit->list,
								   drop);
				double iog_pre_ns = iog_decision +
						    post * materialize_ns;
				double compact_pre_ns = compact_decision +
							post * materialize_ns;
				double chain_pre_ns = chain_decision +
						      post * materialize_ns;
				double list_pre_ns = list_decision +
						     post * materialize_ns;
				double postdrop_ns = materialize_ns + list_decision;
				double rec_bytes =
					(double)ringbuf_record_bytes(copies[p].payload);
				double intermediate_bytes =
					(double)postdrop_intermediate_bytes(copies[p].payload);
				double before_bps = (double)rates[r] * rec_bytes;
				double after_bps = before_bps * post;
				double avoided_intermediate_bps =
					(double)rates[r] * drop * intermediate_bytes;
				double avoided_dirty_cachelines =
					(double)rates[r] * drop *
					(double)cachelines_for_bytes((size_t)rec_bytes);
				double iog_cores = (double)rates[r] * iog_pre_ns / 1e9;
				double compact_cores =
					(double)rates[r] * compact_pre_ns / 1e9;
				double postdrop_cores =
					(double)rates[r] * postdrop_ns / 1e9;
				double chain_cores =
					(double)rates[r] * chain_pre_ns / 1e9;
				double list_cores =
					(double)rates[r] * list_pre_ns / 1e9;
				double saved = postdrop_cores - iog_cores;
				double compact_saved =
					postdrop_cores - compact_cores;

				printf("| %zu | %s | %u | %.0f | %u | %.2f | %.2f | %.2f | %.2f | %.2f | %.1f | %.3f | %.1f | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f | %.2f | %.2f |\n",
				       prefixes, neg_case, rates[r], drop * 100.0,
				       copies[p].payload,
				       before_bps / 1000000.0,
				       after_bps / 1000000.0,
				       before_bps / after_bps,
				       avoided_intermediate_bps / 1000000.0,
				       avoided_dirty_cachelines / 1000000.0,
				       iog_pre_ns,
				       iog_cores,
				       compact_pre_ns,
				       compact_cores,
				       postdrop_cores,
				       chain_cores,
				       list_cores,
				       saved < 0.0 ? 0.0 : saved,
				       compact_saved < 0.0 ? 0.0 : compact_saved,
				       list_pre_ns / iog_pre_ns,
				       list_pre_ns / compact_pre_ns);
			}
		}
	}
}

static int parse_counts(const char *arg, size_t *counts, size_t *nr)
{
	char *copy = xstrdup(arg);
	char *tok, *save = NULL;
	size_t n = 0;

	for (tok = strtok_r(copy, ",", &save); tok;
	     tok = strtok_r(NULL, ",", &save)) {
		char *end = NULL;
		unsigned long value;

		if (n >= 8) {
			free(copy);
			return -E2BIG;
		}

		errno = 0;
		value = strtoul(tok, &end, 10);
		if (errno || !end || *end || !value) {
			free(copy);
			return -EINVAL;
		}
		counts[n++] = value;
	}

	free(copy);
	*nr = n;
	return n ? 0 : -EINVAL;
}

static int parse_dataset(const char *arg, enum workload_kind *kind)
{
	if (!strcmp(arg, "typical"))
		*kind = WORKLOAD_TYPICAL;
	else if (!strcmp(arg, "shared-prefix"))
		*kind = WORKLOAD_SHARED_PREFIX;
	else if (!strcmp(arg, "long-path"))
		*kind = WORKLOAD_LONG_PATH;
	else
		return -EINVAL;
	return 0;
}

static void usage(const char *argv0)
{
	printf("usage: %s [--counts 100,1000,10000] [--dataset typical|shared-prefix|long-path] [--iters N]\n",
	       argv0);
}

int main(int argc, char **argv)
{
	size_t counts[8] = { 100, 1000, 10000 };
	size_t count_nr = 3;
	enum workload_kind dataset = WORKLOAD_TYPICAL;
	u64 iterations_override = 0;
	char perf_status[256];
	size_t ci;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--counts")) {
			if (++i == argc || parse_counts(argv[i], counts, &count_nr)) {
				usage(argv[0]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--dataset")) {
			if (++i == argc || parse_dataset(argv[i], &dataset)) {
				usage(argv[0]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--iters")) {
			char *end = NULL;

			if (++i == argc) {
				usage(argv[0]);
				return 2;
			}
			iterations_override = strtoull(argv[i], &end, 10);
			if (!iterations_override || !end || *end) {
				usage(argv[0]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	printf("io_graph C userspace pre-ringbuf benchmark\n");
	perf_describe(perf_status, sizeof(perf_status));
	printf("perf_counters=%s\n", perf_status);
	printf("invariant=state,cursor,action_code\n");
	printf("execution_order=read_raw_selector_bytes_then_materialize_only_POST\n");
	printf("path_bytes=prebuilt policy_primitive_excludes_path_generation\n");
	printf("decision_quantiles=batch_ns_per_op samples_up_to=%u\n",
	       QUANTILE_NR);
	printf("dataset=%s\n", workload_kind_name(dataset));
	print_io_accounting_rows();

	for (ci = 0; ci < count_nr; ci++) {
		struct workload wl;
		struct iog_compile_stats stats;
		struct iog_map map;
		struct iog_bpf_map *bpf_map = NULL;
		struct iog_cgraph *compact = NULL;
		struct iog_layout_stats layout;
		struct iog_cgraph_stats compact_stats;
		struct run_ctx ctx;
		struct case_result early, late, hit;
		struct bench_result exact_match, compact_exact_match;
		struct bench_result prefix_first_action;
		struct payload_result copy_res[ARRAY_SIZE(payloads)];
		struct update_result update;
		struct map_ops_result map_ops;
		u32 verifier_cases;
		u64 iterations = iterations_override ? iterations_override :
				 (counts[ci] <= 100 ? 1000000ull :
				  counts[ci] <= 1000 ? 300000ull : 32768ull);
		u64 update_iters = counts[ci] <= 100 ? 100ull :
				   counts[ci] <= 1000 ? 20ull : 5ull;
		void *blob = NULL;
		size_t blob_len = 0;
		char err[256] = "";
		struct iog_bpf_attr bpf_attr;
		u32 bpf_key = 0;
		int ret;
		size_t i;

		workload_init(&wl, counts[ci], dataset);
		ret = iog_compile_prefixes(wl.prefixes, wl.nr, 4096, &blob,
					   &blob_len, &stats, err, sizeof(err));
		if (ret) {
			fprintf(stderr, "compile failed: %s\n", err);
			return 1;
		}

		verifier_cases = run_verifier_selftests(blob, blob_len);
		map_ops = exercise_bpf_map_ops(blob, blob_len);
		iog_map_init(&map);
		ret = iog_map_update_blob(&map, blob, blob_len,
					  &iog_default_limits, err, sizeof(err));
		if (ret) {
			fprintf(stderr, "map update failed: %s\n", err);
			return 1;
		}
		ret = iog_map_layout_stats(&map, &layout);
		if (ret) {
			fprintf(stderr, "layout stats failed: %d\n", ret);
			return 1;
		}
		ret = iog_cgraph_new(iog_map_active_graph(&map), &compact);
		if (ret) {
			fprintf(stderr, "compact graph build failed: %d\n", ret);
			return 1;
		}
		ret = iog_cgraph_stats(compact, &compact_stats);
		if (ret) {
			fprintf(stderr, "compact stats failed: %d\n", ret);
			return 1;
		}
		memset(&bpf_attr, 0, sizeof(bpf_attr));
		bpf_attr.key_size = IOG_BPF_KEY_SIZE;
		bpf_attr.value_size = (u32)blob_len;
		bpf_attr.max_entries = IOG_BPF_MAX_ENTRIES;
		ret = iog_bpf_map_ops.map_alloc(&bpf_attr, &bpf_map);
		if (ret) {
			fprintf(stderr, "bpf map alloc failed: %d\n", ret);
			return 1;
		}
		ret = (int)iog_bpf_map_ops.map_update_elem(bpf_map, &bpf_key,
							   blob, IOG_BPF_ANY,
							   err, sizeof(err));
		if (ret) {
			fprintf(stderr, "bpf map update failed: %s\n", err);
			return 1;
		}
		update = bench_update(blob, blob_len, update_iters);

		ctx.map = &map;
		ctx.bpf_map = bpf_map;
		ctx.compact = compact;
		ctx.prefixes = wl.prefixes;
		ctx.prefix_nr = wl.nr;
		assert_equiv(&ctx, wl.early, SAMPLE_NR);
		assert_equiv(&ctx, wl.late, SAMPLE_NR);
		assert_equiv(&ctx, wl.exact, SAMPLE_NR);
		assert_equiv(&ctx, wl.hit, SAMPLE_NR);

		printf("case prefixes=%zu iterations=%" PRIu64 "\n", counts[ci],
		       iterations);
		printf("\nartifact sizes\n");
		printf("| prefixes | avg_len | max_prefix_len | max_probe_len | states | edges | iog_blob_B | compact_runtime_B | dense_table_B | gen_chain_src_B | gen_chain_bpf_est_B | list_payload_B | dense/iog | gen_bpf/iog | blob/compact |\n");
		printf("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
		printf("| %zu | %.1f | %" PRIu32 " | %" PRIu32 " | %" PRIu32 " | %" PRIu32 " | %" PRIu64 " | %" PRIu64 " | %" PRIu64 " | %" PRIu64 " | %" PRIu64 " | %" PRIu64 " | %.1f | %.1f | %.2f |\n",
		       counts[ci],
		       (double)wl.prefix_bytes / (double)wl.nr,
		       stats.max_prefix_len, stats.max_probe_len,
		       stats.node_cnt, stats.edge_cnt, stats.blob_bytes,
		       compact_stats.mem_bytes, stats.dense_table_bytes,
		       stats.gen_chain_source_bytes,
		       stats.gen_chain_bpf_bytes, stats.list_payload_bytes,
		       (double)stats.dense_table_bytes / (double)stats.blob_bytes,
		       (double)stats.gen_chain_bpf_bytes / (double)stats.blob_bytes,
		       compact_stats.mem_bytes ?
		       (double)stats.blob_bytes / (double)compact_stats.mem_bytes :
		       0.0);

		printf("\nverifier selftests\n");
		printf("| prefixes | negative_cases | result |\n");
		printf("|---:|---:|:---|\n");
		printf("| %zu | %" PRIu32 " | ok |\n", counts[ci],
		       verifier_cases);

		printf("\ngraph layout\n");
		printf("| prefixes | zero_edge_nodes | single_edge_nodes | small_fanout_nodes | binary_fanout_nodes | max_fanout |\n");
		printf("|---:|---:|---:|---:|---:|---:|\n");
		printf("| %zu | %" PRIu32 " | %" PRIu32 " | %" PRIu32 " | %" PRIu32 " | %" PRIu32 " |\n",
		       counts[ci], layout.zero_edge_nodes,
		       layout.single_edge_nodes, layout.small_fanout_nodes,
		       layout.binary_fanout_nodes, layout.max_fanout);

		printf("\ncompact runtime graph\n");
		printf("| prefixes | compact_nodes | compact_edges | literal_edges | literal_bytes | mean_literal_len | max_literal_len | max_fanout | max_compact_depth | compact_runtime_B |\n");
		printf("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
		printf("| %zu | %" PRIu32 " | %" PRIu32 " | %" PRIu32 " | %" PRIu32 " | %.2f | %" PRIu32 " | %" PRIu32 " | ",
		       counts[ci], compact_stats.nodes, compact_stats.edges,
		       compact_stats.literal_edges, compact_stats.literal_bytes,
		       compact_stats.literal_edges ?
		       (double)compact_stats.literal_bytes /
		       (double)compact_stats.literal_edges : 0.0,
		       compact_stats.max_literal_len, compact_stats.max_fanout);
		if (compact_stats.depth_complete)
			printf("%" PRIu32, compact_stats.max_depth);
		else
			printf("cyclic");
		printf(" | %" PRIu64 " |\n", compact_stats.mem_bytes);

		printf("\nbpf map update\n");
		printf("| prefixes | verify_us | compact_build_us | map_update_us | update_iters | active_blob_B | active_compact_B | active_total_B | update_scratch_B | peak_new_update_B | retired_graphs | retired_mem_B | peak_with_retired_B | total_mem_B | reclaim_us | reclaimed_graphs | update_seq |\n");
		printf("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
		printf("| %zu | %.2f | %.2f | %.2f | %" PRIu64 " | %" PRIu64 " | %" PRIu64 " | %" PRIu64 " | %" PRIu64 " | %" PRIu64 " | %" PRIu32 " | %" PRIu64 " | %" PRIu64 " | %" PRIu64 " | %.2f | %" PRIu32 " | %" PRIu64 " |\n",
		       counts[ci], update.verify_us, update.compact_build_us,
		       update.update_us, update_iters, update.active_blob_bytes,
		       update.active_compact_bytes, update.active_mem_bytes,
		       update.update_scratch_bytes, update.peak_new_update_bytes,
		       update.retired_graphs, update.retired_mem_bytes,
		       update.peak_with_retired_bytes,
		       update.total_mem_bytes, update.reclaim_us,
		       update.reclaimed_graphs, update.update_seq);

		printf("\nbpf map ops\n");
		printf("| prefixes | key_size | value_size | max_entries | update_ret | lookup_ret | get_next_key_ret | delete_ret | mem_usage_B | action_only_update_ret | action_only_mem_usage_B | action_only_run_action | action_only_run_ret | update_seq | retired_after_delete | reclaimed_graphs |\n");
		printf("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
		printf("| %zu | %u | %zu | %u | %ld | %ld | %ld | %ld | %" PRIu64 " | %ld | %" PRIu64 " | %" PRIu32 " | %ld | %" PRIu64 " | %" PRIu32 " | %" PRIu32 " |\n",
		       counts[ci], IOG_BPF_KEY_SIZE, blob_len,
		       IOG_BPF_MAX_ENTRIES, map_ops.update_ret,
		       map_ops.lookup_unsupported, map_ops.next_key_unsupported,
		       map_ops.delete_ret, map_ops.mem_usage_bytes,
		       map_ops.action_only_update_ret,
		       map_ops.action_only_mem_usage_bytes,
		       map_ops.action_only_run_action,
		       map_ops.action_only_run_ret, map_ops.update_seq,
		       map_ops.retired_graphs_after_delete,
		       map_ops.reclaimed_graphs);

		early.iog = bench_match(&ctx, wl.early, SAMPLE_NR,
					match_iog, iterations);
		early.compact = bench_match(&ctx, wl.early, SAMPLE_NR,
					    match_iog_compact, iterations);
		early.chain = bench_match(&ctx, wl.early, SAMPLE_NR,
					  match_chain, iterations);
		early.list = bench_match(&ctx, wl.early, SAMPLE_NR,
					 match_list, iterations);
		late.iog = bench_match(&ctx, wl.late, SAMPLE_NR,
				       match_iog, iterations);
		late.compact = bench_match(&ctx, wl.late, SAMPLE_NR,
					   match_iog_compact, iterations);
		late.chain = bench_match(&ctx, wl.late, SAMPLE_NR,
					 match_chain, iterations);
		late.list = bench_match(&ctx, wl.late, SAMPLE_NR,
					match_list, iterations);
		hit.iog = bench_match(&ctx, wl.hit, SAMPLE_NR,
				      match_iog, iterations);
		hit.compact = bench_match(&ctx, wl.hit, SAMPLE_NR,
					  match_iog_compact, iterations);
		hit.chain = bench_match(&ctx, wl.hit, SAMPLE_NR,
					match_chain, iterations);
		hit.list = bench_match(&ctx, wl.hit, SAMPLE_NR,
				       match_list, iterations);
		exact_match = bench_match(&ctx, wl.exact, SAMPLE_NR,
					  match_iog, iterations);
		compact_exact_match = bench_match(&ctx, wl.exact, SAMPLE_NR,
						  match_iog_compact,
						  iterations);
		prefix_first_action = bench_match(&ctx, wl.hit, SAMPLE_NR,
						  match_iog_first_action,
						  iterations);

		printf("\n");
		printf("decision cost\n");
		printf("| prefixes | case | matcher | mean_ns/op | batch_p95_ns/op | batch_p99_ns/op | batch_p999_ns/op | cycles/op | branch_miss/op | l1d_miss/op | llc_miss/op | run_allocs |\n");
		printf("|---:|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");

		print_decision_row(counts[ci], "early_miss", "io_graph_byte_trie",
				   &early.iog);
		print_decision_row(counts[ci], "early_miss",
				   "io_graph_compact_chain", &early.compact);
		print_decision_row(counts[ci], "early_miss", "gen_chain",
				   &early.chain);
		print_decision_row(counts[ci], "early_miss", "list_loop",
				   &early.list);
		print_decision_row(counts[ci], "late_miss", "io_graph_byte_trie",
				   &late.iog);
		print_decision_row(counts[ci], "late_miss",
				   "io_graph_compact_chain", &late.compact);
		print_decision_row(counts[ci], "late_miss", "gen_chain",
				   &late.chain);
		print_decision_row(counts[ci], "late_miss", "list_loop",
				   &late.list);
		print_decision_row(counts[ci], "hit", "io_graph_byte_trie",
				   &hit.iog);
		print_decision_row(counts[ci], "hit",
				   "io_graph_compact_chain", &hit.compact);
		print_decision_row(counts[ci], "hit", "gen_chain", &hit.chain);
		print_decision_row(counts[ci], "hit", "list_loop", &hit.list);

		printf("\nmatched path cost\n");
		printf("matched_transition=successful graph state advance; prefix rows keep suffix bytes in input_B/op\n");
		printf("| prefixes | case | matcher | input_B/op | matched_transitions/op | mean_ns/op | mean_ns/input_B | mean_ns/matched_transition | batch_p95_ns/op | batch_p99_ns/op | batch_p999_ns/op | cycles/op | branch_miss/op | l1d_miss/op | llc_miss/op |\n");
		printf("|---:|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
		print_matched_path_row(counts[ci],
				       dataset == WORKLOAD_LONG_PATH ?
				       "exact_long_match" : "exact_short_match",
				       "io_graph_byte_trie_last_accept",
				       &exact_match,
				       sample_mean_len(wl.exact, SAMPLE_NR),
				       sample_mean_len(wl.exact, SAMPLE_NR));
		print_matched_path_row(counts[ci],
				       dataset == WORKLOAD_LONG_PATH ?
				       "exact_long_match" : "exact_short_match",
				       "io_graph_compact_chain",
				       &compact_exact_match,
				       sample_mean_len(wl.exact, SAMPLE_NR),
				       sample_mean_compact_transitions(
					       compact, wl.exact, SAMPLE_NR));
		print_matched_path_row(counts[ci],
				       "prefix_accept_early_return",
				       "io_graph_byte_trie_first_final_action",
				       &prefix_first_action,
				       sample_mean_len(wl.hit, SAMPLE_NR),
				       sample_mean_len(wl.exact, SAMPLE_NR));
		print_matched_path_row(counts[ci], "prefix_longest_match",
				       "io_graph_byte_trie_last_accept", &hit.iog,
				       sample_mean_len(wl.hit, SAMPLE_NR),
				       sample_mean_len(wl.exact, SAMPLE_NR));
		print_matched_path_row(counts[ci], "prefix_longest_match",
				       "io_graph_compact_chain", &hit.compact,
				       sample_mean_len(wl.hit, SAMPLE_NR),
				       sample_mean_compact_transitions(
					       compact, wl.hit, SAMPLE_NR));

		printf("\nselector acquisition + compact decision, prefixes=%zu\n",
		       counts[ci]);
		printf("acquisition_model=bounded_memcpy_before_run_action max_probe_len=min(max_input_len,max_prefix_len+1)\n");
		printf("| prefixes | case | acquisition | probe_len | copied_B/op | ns/op | cycles/op | run_allocs |\n");
		printf("|---:|:---|:---|---:|---:|---:|---:|---:|\n");
		{
			struct bench_result acq;
			double copied_b_op;

			acq = bench_selector_acquire_decision(&ctx, wl.early,
							      SAMPLE_NR, 256,
							      false, iterations,
							      &copied_b_op);
			print_selector_acquisition_row(counts[ci], "early_miss",
						       "full_selector_copy",
						       256, &acq, copied_b_op);
			acq = bench_selector_acquire_decision(&ctx, wl.early,
							      SAMPLE_NR,
							      stats.max_probe_len,
							      true, iterations,
							      &copied_b_op);
			print_selector_acquisition_row(counts[ci], "early_miss",
						       "bounded_probe_copy",
						       stats.max_probe_len,
						       &acq, copied_b_op);

			acq = bench_selector_acquire_decision(&ctx, wl.late,
							      SAMPLE_NR, 256,
							      false, iterations,
							      &copied_b_op);
			print_selector_acquisition_row(counts[ci], "late_miss",
						       "full_selector_copy",
						       256, &acq, copied_b_op);
			acq = bench_selector_acquire_decision(&ctx, wl.late,
							      SAMPLE_NR,
							      stats.max_probe_len,
							      true, iterations,
							      &copied_b_op);
			print_selector_acquisition_row(counts[ci], "late_miss",
						       "bounded_probe_copy",
						       stats.max_probe_len,
						       &acq, copied_b_op);

			acq = bench_selector_acquire_decision(&ctx, wl.hit,
							      SAMPLE_NR, 256,
							      false, iterations,
							      &copied_b_op);
			print_selector_acquisition_row(counts[ci], "hit",
						       "full_selector_copy",
						       256, &acq, copied_b_op);
			acq = bench_selector_acquire_decision(&ctx, wl.hit,
							      SAMPLE_NR,
							      stats.max_probe_len,
							      true, iterations,
							      &copied_b_op);
			print_selector_acquisition_row(counts[ci], "hit",
						       "bounded_probe_copy",
						       stats.max_probe_len,
						       &acq, copied_b_op);
		}

		for (i = 0; i < ARRAY_SIZE(payloads); i++) {
			copy_res[i].payload = payloads[i];
			copy_res[i].materialize =
				bench_materialize(payloads[i], iterations);
		}

		printf("\nmaterialization+decode cost, prefixes=%zu\n", counts[ci]);
		printf("| payload_B | record_B | materialize_ns/op | materialize_cycles/op |\n");
		printf("|---:|---:|---:|---:|\n");
		for (i = 0; i < ARRAY_SIZE(copy_res); i++) {
			printf("| %u | %zu | %.2f | ", copy_res[i].payload,
			       ringbuf_record_bytes(copy_res[i].payload),
			       copy_res[i].materialize.ns_op);
			print_metric(copy_res[i].materialize.cycles_op);
			printf(" |\n");
		}

		printf("\nbpf event path (compact run_action shim), prefixes=%zu\n",
		       counts[ci]);
		printf("| prefixes | case | payload_B | ns/op | cycles/op | branch_miss/op | l1d_miss/op | llc_miss/op | emitted_ringbuf_B/op | reserve_fail/op | run_allocs |\n");
		printf("|---:|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
		for (i = 0; i < ARRAY_SIZE(payloads); i++) {
			struct bench_result bpf;
			double emitted_b_op, reserve_fail_op;

			bpf = bench_bpf_event_path(&ctx, wl.early, SAMPLE_NR,
						   payloads[i], iterations,
						   &emitted_b_op,
						   &reserve_fail_op);
			print_bpf_event_path_row(counts[ci], "early_miss",
						 payloads[i], &bpf,
						 emitted_b_op,
						 reserve_fail_op);

			bpf = bench_bpf_event_path(&ctx, wl.late, SAMPLE_NR,
						   payloads[i], iterations,
						   &emitted_b_op,
						   &reserve_fail_op);
			print_bpf_event_path_row(counts[ci], "late_miss",
						 payloads[i], &bpf,
						 emitted_b_op,
						 reserve_fail_op);

			bpf = bench_bpf_event_path(&ctx, wl.hit, SAMPLE_NR,
						   payloads[i], iterations,
						   &emitted_b_op,
						   &reserve_fail_op);
			print_bpf_event_path_row(counts[ci], "hit",
						 payloads[i], &bpf,
						 emitted_b_op,
						 reserve_fail_op);
		}

		printf("\nringbuf drop order, prefixes=%zu\n", counts[ci]);
		printf("| prefixes | case | payload_B | order | ns/op | cycles/op | reserved_ringbuf_B/op | emitted_ringbuf_B/op | reserve_fail/op | run_allocs |\n");
		printf("|---:|:---|---:|:---|---:|---:|---:|---:|---:|---:|\n");
		for (i = 0; i < ARRAY_SIZE(payloads); i++) {
			struct bench_result row;
			double reserved_b_op, emitted_b_op, reserve_fail_op;

			row = bench_bpf_event_path(&ctx, wl.early, SAMPLE_NR,
						   payloads[i], iterations,
						   &emitted_b_op,
						   &reserve_fail_op);
			print_drop_order_row(counts[ci], "early_miss",
					     payloads[i], "drop_before_reserve",
					     &row, emitted_b_op, emitted_b_op,
					     reserve_fail_op);
			row = bench_discard_after_reserve(&ctx, wl.early,
							  SAMPLE_NR,
							  payloads[i],
							  iterations,
							  &reserved_b_op,
							  &emitted_b_op,
							  &reserve_fail_op);
			print_drop_order_row(counts[ci], "early_miss",
					     payloads[i],
					     "discard_after_reserve",
					     &row, reserved_b_op, emitted_b_op,
					     reserve_fail_op);

			row = bench_bpf_event_path(&ctx, wl.late, SAMPLE_NR,
						   payloads[i], iterations,
						   &emitted_b_op,
						   &reserve_fail_op);
			print_drop_order_row(counts[ci], "late_miss",
					     payloads[i], "drop_before_reserve",
					     &row, emitted_b_op, emitted_b_op,
					     reserve_fail_op);
			row = bench_discard_after_reserve(&ctx, wl.late,
							  SAMPLE_NR,
							  payloads[i],
							  iterations,
							  &reserved_b_op,
							  &emitted_b_op,
							  &reserve_fail_op);
			print_drop_order_row(counts[ci], "late_miss",
					     payloads[i],
					     "discard_after_reserve",
					     &row, reserved_b_op, emitted_b_op,
					     reserve_fail_op);

			row = bench_bpf_event_path(&ctx, wl.hit, SAMPLE_NR,
						   payloads[i], iterations,
						   &emitted_b_op,
						   &reserve_fail_op);
			print_drop_order_row(counts[ci], "hit", payloads[i],
					     "drop_before_reserve", &row,
					     emitted_b_op, emitted_b_op,
					     reserve_fail_op);
			row = bench_discard_after_reserve(&ctx, wl.hit,
							  SAMPLE_NR,
							  payloads[i],
							  iterations,
							  &reserved_b_op,
							  &emitted_b_op,
							  &reserve_fail_op);
			print_drop_order_row(counts[ci], "hit", payloads[i],
					     "discard_after_reserve", &row,
					     reserved_b_op, emitted_b_op,
					     reserve_fail_op);
		}

		printf("\npre-ringbuf estimate, prefixes=%zu\n", counts[ci]);
		printf("| prefixes | neg_case | events/sec | drop_pct | payload_B | before_ringbuf_MB/s | after_ringbuf_MB/s | traffic_reduction | avoided_intermediate_MB/s | avoided_dirty_cachelines_M/s | iog_prefilter_ns | iog_cores | compact_prefilter_ns | compact_cores | postdrop_cores | gen_chain_prefilter_cores | list_prefilter_cores | cores_saved_vs_postdrop | compact_cores_saved_vs_postdrop | speedup_vs_list_prefilter | compact_speedup_vs_list_prefilter |\n");
		printf("|---:|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
		print_estimate_rows(counts[ci], "early_miss", &early, &hit,
				    copy_res, ARRAY_SIZE(copy_res));
		print_estimate_rows(counts[ci], "late_miss", &late, &hit,
				    copy_res, ARRAY_SIZE(copy_res));

		free(blob);
		iog_cgraph_free(compact);
		iog_bpf_map_ops.map_free(bpf_map);
		iog_map_destroy(&map);
		workload_free(&wl);
	}

	return 0;
}
