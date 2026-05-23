// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define IOGRAPH_BENCH_SELECTOR_CAP	256
#define IOGRAPH_LPM_MAX_ENTRIES		16384

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
};

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 1 << 20);
} events SEC(".maps");

const volatile __u32 selector_len;
const volatile __u32 drop_action = 1;
__u8 selector[IOGRAPH_BENCH_SELECTOR_CAP];

long posts;
long reserve_fails;

extern __u32 bpf_iograph_run_action(struct bpf_map *map, const __u8 *buf,
				    __u32 len, __u32 entry) __ksym;

static __always_inline int iograph_emit_action(__u32 action)
{
	struct iograph_bench_event *event;

	if (action == drop_action)
		return 0;

	__sync_fetch_and_add(&posts, 1);
	event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
	if (!event) {
		__sync_fetch_and_add(&reserve_fails, 1);
		return 0;
	}
	event->action_code = action;
	bpf_ringbuf_submit(event, 0);
	return 0;
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
int iograph_lpm_decision_bench_run(struct bpf_raw_tracepoint_args *ctx)
{
	__u32 len = selector_len;

	(void)ctx;
	if (len > sizeof(selector))
		return 0;

	return iograph_lpm_action(len) != 0;
}

char LICENSE[] SEC("license") = "GPL";
