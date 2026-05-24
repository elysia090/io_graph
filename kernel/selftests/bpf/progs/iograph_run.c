// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "iograph_test.h"

struct {
	__uint(type, BPF_MAP_TYPE_IOGRAPH);
	__uint(max_entries, 1);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, sizeof(struct iograph_test_blob));
} iograph_policy SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct iograph_test_result);
} result_map SEC(".maps");

struct iograph_event {
	__u32 action_code;
	__u32 final_state;
};

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 4096);
} events SEC(".maps");

extern int bpf_iograph_run(struct bpf_map *map, const __u8 *buf, __u32 len,
			   __u32 entry,
			   struct bpf_iograph_run_result *run) __ksym;
extern __u32 bpf_iograph_run_action(struct bpf_map *map, const __u8 *buf,
				    __u32 len, __u32 entry) __ksym;
extern __u32 bpf_iograph_run_action_idx(struct bpf_map *map, const __u8 *buf,
					__u32 len, __u32 entry_idx) __ksym;

static __u8 test_path[] = IOGRAPH_TEST_PATH;

SEC("tp_btf/sys_enter")
int BPF_PROG(iograph_run_before_ringbuf, struct pt_regs *regs, long id)
{
	struct iograph_test_result *result;
	struct iograph_event *event;
	struct bpf_iograph_run_result run = {};
	__u32 key = 0;
	int ret;

	(void)regs;
	(void)id;
	result = bpf_map_lookup_elem(&result_map, &key);
	if (!result)
		return 0;

	ret = bpf_iograph_run((struct bpf_map *)&iograph_policy, test_path,
			      sizeof(test_path) - 1, 0, &run);
	result->kfunc_ret = ret;
	result->final_state = run.final_state;
	result->action_code = run.action_code;
	result->run_action_code =
		bpf_iograph_run_action((struct bpf_map *)&iograph_policy,
				       test_path, sizeof(test_path) - 1, 0);
	result->run_action_idx_code =
		bpf_iograph_run_action_idx((struct bpf_map *)&iograph_policy,
					   test_path, sizeof(test_path) - 1, 0);

	if (ret || run.action_code == IOGRAPH_TEST_DROP)
		return 0;

	event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
	if (!event)
		return 0;

	event->action_code = run.action_code;
	event->final_state = run.final_state;
	result->ringbuf_reserve_seen = 1;
	bpf_ringbuf_submit(event, 0);
	return 0;
}

char LICENSE[] SEC("license") = "GPL";
