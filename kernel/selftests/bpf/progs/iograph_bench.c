// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define IOGRAPH_BENCH_SELECTOR_CAP	256
#define IOGRAPH_LPM_MAX_ENTRIES		16384
#define IOGRAPH_BENCH_MAX_PAYLOAD	2048

struct {
	__uint(type, BPF_MAP_TYPE_IOGRAPH);
	__uint(max_entries, 1);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, 4096);
} policy SEC(".maps");

struct iograph_lpm_key {
	__u32 prefixlen;
	__u8 data[IOGRAPH_BENCH_SELECTOR_CAP];
};

struct {
	__uint(type, BPF_MAP_TYPE_LPM_TRIE);
	__uint(max_entries, IOGRAPH_LPM_MAX_ENTRIES);
	__type(key, struct iograph_lpm_key);
	__type(value, __u32);
	__uint(map_flags, BPF_F_NO_PREALLOC);
} lpm_policy SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct iograph_lpm_key);
} lpm_scratch SEC(".maps");

struct iograph_bench_event {
	__u32 action_code;
	__u32 payload_len;
	__u8 payload[];
};

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 1 << 20);
} events SEC(".maps");

const volatile __u32 selector_len;
const volatile __u32 probe_len;
const volatile __u32 drop_action = 1;
const volatile __u32 payload_len;
__u8 selector[IOGRAPH_BENCH_SELECTOR_CAP];
__u8 payload[IOGRAPH_BENCH_MAX_PAYLOAD];

long posts;
long reserve_fails;

extern __u32 bpf_iograph_run_action(struct bpf_map *map, const __u8 *buf,
				    __u32 len, __u32 entry) __ksym;
extern __u32 bpf_iograph_run_action_idx(struct bpf_map *map, const __u8 *buf,
					__u32 len, __u32 entry_idx) __ksym;

#define IOGRAPH_EMIT_PAYLOAD_CONST(_len) do {				\
	event = bpf_ringbuf_reserve(&events, sizeof(*event) + (_len), 0); \
	if (!event) {							\
		__sync_fetch_and_add(&reserve_fails, 1);		\
		return 0;						\
	}								\
	__sync_fetch_and_add(&posts, 1);				\
	event->action_code = action;					\
	event->payload_len = (_len);					\
	for (i = 0; i < (_len); i++)					\
		event->payload[i] = payload[i];				\
	bpf_ringbuf_submit(event, 0);					\
	return 0;							\
} while (0)

static __always_inline int iograph_emit_action(__u32 action)
{
	struct iograph_bench_event *event;

	if (action == drop_action)
		return 0;

	event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
	if (!event) {
		__sync_fetch_and_add(&reserve_fails, 1);
		return 0;
	}
	__sync_fetch_and_add(&posts, 1);
	event->action_code = action;
	event->payload_len = 0;
	bpf_ringbuf_submit(event, 0);
	return 0;
}

static __always_inline int iograph_emit_payload_action(__u32 action)
{
	struct iograph_bench_event *event;
	__u32 len = payload_len;
	__u32 i;

	if (action == drop_action)
		return 0;

	if (len == 300)
		IOGRAPH_EMIT_PAYLOAD_CONST(300);
	if (len == 800)
		IOGRAPH_EMIT_PAYLOAD_CONST(800);
	IOGRAPH_EMIT_PAYLOAD_CONST(2048);
}

static __always_inline __u32 iograph_effective_probe_len(__u32 len)
{
	__u32 cap = probe_len;

	if (cap && len > cap)
		len = cap;
	return len;
}

static __always_inline __u32 iograph_copy_selector(__u8 *dst, __u32 len)
{
	int i;

	len = iograph_effective_probe_len(len);
#pragma unroll
	for (i = 0; i < IOGRAPH_BENCH_SELECTOR_CAP; i++) {
		if ((__u32)i >= len)
			break;
		dst[i] = selector[i];
	}
	return len;
}

SEC("raw_tp/sys_enter")
int iograph_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	__u32 len = selector_len;
	__u32 action;

	(void)ctx;
	if (len > sizeof(selector))
		return 0;

	action = bpf_iograph_run_action((struct bpf_map *)&policy, selector,
					len, 0);
	return iograph_emit_action(action);
}

SEC("raw_tp/sys_enter")
int iograph_payload_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	__u32 len = selector_len;
	__u32 action;

	(void)ctx;
	if (len > sizeof(selector))
		return 0;

	action = bpf_iograph_run_action((struct bpf_map *)&policy, selector,
					len, 0);
	return iograph_emit_payload_action(action);
}

SEC("raw_tp/sys_enter")
int iograph_always_post_payload_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	(void)ctx;
	return iograph_emit_payload_action(drop_action == ~0u ?
					   0 : drop_action + 1);
}

SEC("raw_tp/sys_enter")
int iograph_hook_floor_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	(void)ctx;
	return 0;
}

SEC("raw_tp/sys_enter")
int iograph_decision_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	__u32 len = selector_len;
	__u32 action;

	(void)ctx;
	if (len > sizeof(selector))
		return 0;

	action = bpf_iograph_run_action((struct bpf_map *)&policy, selector,
					len, 0);
	return action != 0;
}

SEC("raw_tp/sys_enter")
int iograph_idx_decision_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	__u32 len = selector_len;
	__u32 action;

	(void)ctx;
	if (len > sizeof(selector))
		return 0;

	action = bpf_iograph_run_action_idx((struct bpf_map *)&policy,
					    selector, len, 0);
	return action != 0;
}

SEC("raw_tp/sys_enter")
int iograph_acquire_decision_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	__u8 buf[IOGRAPH_BENCH_SELECTOR_CAP];
	__u32 len = selector_len;
	__u32 action;

	(void)ctx;
	if (len > sizeof(selector))
		return 0;

	len = iograph_copy_selector(buf, len);
	action = bpf_iograph_run_action((struct bpf_map *)&policy, buf,
					len, 0);
	return action != 0;
}

SEC("raw_tp/sys_enter")
int iograph_discard_after_reserve_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	struct iograph_bench_event *event;
	__u32 len = selector_len;
	__u32 action;

	(void)ctx;
	if (len > sizeof(selector))
		return 0;

	action = bpf_iograph_run_action((struct bpf_map *)&policy, selector,
					len, 0);
	if (action == drop_action) {
		event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
		if (!event) {
			__sync_fetch_and_add(&reserve_fails, 1);
			return 0;
		}
		bpf_ringbuf_discard(event, 0);
		return 0;
	}

	return iograph_emit_action(action);
}

static __always_inline __u32 iograph_lpm_action(__u32 len)
{
	struct iograph_lpm_key *key;
	__u32 zero = 0;
	__u32 *action;

	key = bpf_map_lookup_elem(&lpm_scratch, &zero);
	if (!key)
		return 0;
	key->prefixlen = len * 8u;
	__builtin_memcpy(key->data, selector, sizeof(key->data));
	action = bpf_map_lookup_elem(&lpm_policy, key);
	return action ? *action : 0;
}

static __always_inline __u32
iograph_lpm_bounded_action_from(const __u8 *src, __u32 len)
{
	struct iograph_lpm_key *key;
	__u32 zero = 0;
	__u32 *action;
	int i;

	key = bpf_map_lookup_elem(&lpm_scratch, &zero);
	if (!key)
		return 0;
	key->prefixlen = len * 8u;
#pragma unroll
	for (i = 0; i < IOGRAPH_BENCH_SELECTOR_CAP; i++) {
		if ((__u32)i >= len)
			break;
		key->data[i] = src[i];
	}
	action = bpf_map_lookup_elem(&lpm_policy, key);
	return action ? *action : 0;
}

static __always_inline __u32 iograph_lpm_bounded_action(__u32 len)
{
	return iograph_lpm_bounded_action_from(selector, len);
}

SEC("raw_tp/sys_enter")
int iograph_lpm_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	__u32 len = selector_len;

	(void)ctx;
	if (len > sizeof(selector))
		return 0;

	return iograph_emit_action(iograph_lpm_action(len));
}

SEC("raw_tp/sys_enter")
int iograph_lpm_bounded_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	__u32 len = selector_len;

	(void)ctx;
	if (len > sizeof(selector))
		return 0;

	return iograph_emit_action(iograph_lpm_bounded_action(len));
}

SEC("raw_tp/sys_enter")
int iograph_lpm_decision_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	__u32 len = selector_len;

	(void)ctx;
	if (len > sizeof(selector))
		return 0;

	return iograph_lpm_action(len) != 0;
}

SEC("raw_tp/sys_enter")
int iograph_lpm_bounded_decision_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	__u32 len = selector_len;

	(void)ctx;
	if (len > sizeof(selector))
		return 0;

	return iograph_lpm_bounded_action(len) != 0;
}

SEC("raw_tp/sys_enter")
int iograph_lpm_bounded_acquire_decision_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	__u8 buf[IOGRAPH_BENCH_SELECTOR_CAP];
	__u32 len = selector_len;

	(void)ctx;
	if (len > sizeof(selector))
		return 0;

	len = iograph_copy_selector(buf, len);
	return iograph_lpm_bounded_action_from(buf, len) != 0;
}

char LICENSE[] SEC("license") = "GPL";
