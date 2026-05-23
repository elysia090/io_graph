// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

struct {
	__uint(type, BPF_MAP_TYPE_IOGRAPH);
	__uint(max_entries, 1);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, 65536);
} policy SEC(".maps");

struct io_graph_event {
	__u32 action_code;
};

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 1 << 20);
} events SEC(".maps");

extern __u32 bpf_iograph_run_action(struct bpf_map *map, const __u8 *buf,
				    __u32 len, __u32 entry) __ksym;

SEC("raw_tp/sys_enter")
int prefilter_demo(struct bpf_raw_tracepoint_args *ctx)
{
	static __u8 selector[] = "/drop/event";
	struct io_graph_event *event;
	__u32 action;

	(void)ctx;
	action = bpf_iograph_run_action((struct bpf_map *)&policy, selector,
					sizeof(selector) - 1, 0);
	if (action == 1)
		return 0;

	event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
	if (!event)
		return 0;
	event->action_code = action;
	bpf_ringbuf_submit(event, 0);
	return 0;
}

char LICENSE[] SEC("license") = "GPL";
