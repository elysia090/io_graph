// SPDX-License-Identifier: GPL-2.0
#include <test_progs.h>

#include "iograph_run.skel.h"
#include "progs/iograph_test.h"

static void iograph_build_drop_blob(struct iograph_test_blob *blob)
{
	static const __u8 prefix[] = "/drop/";
	__u32 off, i;

	memset(blob, 0, sizeof(*blob));
	blob->hdr.magic = IOG_MAGIC;
	blob->hdr.version = IOG_VERSION;
	blob->hdr.node_cnt = IOGRAPH_TEST_NODES;
	blob->hdr.edge_cnt = IOGRAPH_TEST_EDGES;
	blob->hdr.entry_cnt = IOGRAPH_TEST_ENTRIES;
	blob->hdr.accept_cnt = IOGRAPH_TEST_ACCEPTS;
	blob->hdr.alphabet_size = 256;
	blob->hdr.initial_state = 0;
	blob->hdr.nodes_off = sizeof(blob->hdr);
	off = blob->hdr.nodes_off + sizeof(blob->nodes);
	blob->hdr.edges_off = off;
	off += sizeof(blob->edges);
	blob->hdr.entries_off = off;
	off += sizeof(blob->entries);
	blob->hdr.accepts_off = off;
	blob->hdr.total_size = sizeof(*blob);
	blob->hdr.max_input_len = 64;

	for (i = 0; i < IOGRAPH_TEST_NODES; i++) {
		blob->nodes[i].edge_start = i;
		blob->nodes[i].default_dst = IOG_NO_STATE;
	}
	for (i = 0; i < IOGRAPH_TEST_EDGES; i++) {
		blob->nodes[i].edge_cnt = 1;
		blob->edges[i].sym_lo = prefix[i];
		blob->edges[i].sym_hi = prefix[i];
		blob->edges[i].dst = i + 1;
	}
	blob->nodes[IOGRAPH_TEST_NODES - 1].edge_start = IOGRAPH_TEST_EDGES;
	blob->nodes[IOGRAPH_TEST_NODES - 1].accept_id = 1;
	blob->nodes[IOGRAPH_TEST_NODES - 1].flags = IOG_NODE_F_FINAL_ACTION;
	blob->entries[0].id = 0;
	blob->entries[0].state = 0;
	blob->accepts[0].id = 0;
	blob->accepts[0].code = IOGRAPH_TEST_NOMATCH;
	blob->accepts[1].id = 1;
	blob->accepts[1].code = IOGRAPH_TEST_DROP;
}

void test_iograph(void)
{
	struct iograph_test_result result = {};
	struct iograph_test_blob blob;
	struct iograph_run *skel;
	__u32 key = 0;
	int err, fd;

	LIBBPF_OPTS(bpf_map_create_opts, opts,
		.map_flags = BPF_F_NUMA_NODE,
		.numa_node = 0,
	);

	fd = bpf_map_create(BPF_MAP_TYPE_IOGRAPH, "iograph_numa",
			    sizeof(__u32), sizeof(blob), 1, &opts);
	if (!ASSERT_LT(fd, 0, "reject_numa_flag")) {
		close(fd);
		return;
	}

	skel = iograph_run__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	iograph_build_drop_blob(&blob);
	blob.hdr.magic ^= 1;
	err = bpf_map_update_elem(bpf_map__fd(skel->maps.iograph_policy), &key,
				  &blob, BPF_ANY);
	if (!ASSERT_NEQ(err, 0, "reject_bad_magic"))
		goto out;

	iograph_build_drop_blob(&blob);
	blob.nodes[0].edge_cnt = 2;
	blob.edges[1].sym_lo = blob.edges[0].sym_lo;
	blob.edges[1].sym_hi = blob.edges[0].sym_hi;
	err = bpf_map_update_elem(bpf_map__fd(skel->maps.iograph_policy), &key,
				  &blob, BPF_ANY);
	if (!ASSERT_NEQ(err, 0, "reject_unsorted_edge"))
		goto out;

	iograph_build_drop_blob(&blob);
	blob.nodes[0].flags = IOG_NODE_F_FINAL_ACTION;
	err = bpf_map_update_elem(bpf_map__fd(skel->maps.iograph_policy), &key,
				  &blob, BPF_ANY);
	if (!ASSERT_NEQ(err, 0, "reject_final_action_without_accept"))
		goto out;

	iograph_build_drop_blob(&blob);
	err = bpf_map_update_elem(bpf_map__fd(skel->maps.iograph_policy), &key,
				  &blob, BPF_ANY);
	if (!ASSERT_OK(err, "policy_update"))
		goto out;

	err = iograph_run__attach(skel);
	if (!ASSERT_OK(err, "attach"))
		goto out;

	syscall(__NR_getpid);

	err = bpf_map_lookup_elem(bpf_map__fd(skel->maps.result_map), &key,
				  &result);
	if (!ASSERT_OK(err, "result_lookup"))
		goto out;

	ASSERT_OK(result.kfunc_ret, "kfunc_ret");
	ASSERT_EQ(result.final_state, IOGRAPH_TEST_NODES - 1, "final_state");
	ASSERT_EQ(result.action_code, IOGRAPH_TEST_DROP, "action_code");
	ASSERT_EQ(result.run_action_code, IOGRAPH_TEST_DROP, "run_action_code");
	ASSERT_EQ(result.run_action_idx_code, IOGRAPH_TEST_DROP,
		  "run_action_idx_code");
	ASSERT_EQ(result.ringbuf_reserve_seen, 0, "drop_before_reserve");

out:
	iograph_run__destroy(skel);
}
