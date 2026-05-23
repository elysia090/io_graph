/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _BPF_IOGRAPH_INTERNAL_H
#define _BPF_IOGRAPH_INTERNAL_H

#include <linux/bpf.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <uapi/linux/bpf_iograph.h>

#define BPF_IOGRAPH_MAX_BLOB_SIZE	(8u << 20)
#define BPF_IOGRAPH_MAX_NODES		250000u
#define BPF_IOGRAPH_MAX_EDGES		500000u
#define BPF_IOGRAPH_MAX_ENTRIES		64u
#define BPF_IOGRAPH_MAX_ACCEPTS		65536u
#define BPF_IOGRAPH_MAX_INPUT_LEN	65536u
#define BPF_IOGRAPH_CEDGE_LITERAL	(1u << 0)
#define BPF_IOGRAPH_CEDGE_FINAL_ACTION	(1u << 1)
#define BPF_IOGRAPH_DISPATCH256_THRESHOLD	16u
#define BPF_IOGRAPH_DISPATCH256_SIZE	256u

struct bpf_iograph_cnode {
	u32 edge_start;
	/* 1-based offset into compact_dispatch; 0 means no dispatch table. */
	u32 dispatch_start;
	u16 edge_cnt;
	u16 flags;
	u32 default_dst;
	u32 action_code;
};

struct bpf_iograph_cedge {
	/*
	 * dst is a compact node id unless BPF_IOGRAPH_CEDGE_FINAL_ACTION is
	 * set, in which case it is the final action code.
	 */
	u32 dst;
	u32 lit_off;
	/* Tail bytes after the first dispatch byte. */
	u32 lit_len;
	u8 sym_lo;
	u8 sym_hi;
	u8 flags;
};

struct bpf_iograph_graph {
	struct rcu_head rcu;
	u32 blob_len;
	u32 max_input_len;
	u32 node_cnt;
	u32 entry_cnt;
	u32 single_entry_id;
	u32 single_entry_state;
	u32 compact_single_entry_state;
	u32 compact_entry_cnt;
	u32 compact_node_cnt;
	u32 compact_edge_cnt;
	u32 compact_lit_len;
	u32 compact_dispatch_cnt;
	u32 compact_literal_edge_cnt;
	u32 compact_max_literal_len;
	u64 compact_mem_bytes;
	bool single_entry;
	const struct iog_blob_hdr *hdr;
	const struct iog_node *nodes;
	const struct iog_edge *edges;
	const struct iog_entry *entries;
	const struct iog_accept *accepts;
	u8 *blob;
	void *compact_data;
	struct iog_entry *compact_entries;
	struct bpf_iograph_cnode *compact_nodes;
	struct bpf_iograph_cedge *compact_edges;
	u16 *compact_dispatch;
	u8 *compact_lits;
};

struct bpf_iograph_map {
	struct bpf_map map;
	struct bpf_iograph_graph __rcu *graph;
	struct mutex update_lock;
	u64 update_seq;
};

__bpf_kfunc int bpf_iograph_run(struct bpf_map *map, const u8 *buf,
				u32 buf__sz, u32 entry_id,
				struct bpf_iograph_run_result *result__uninit);
__bpf_kfunc u32 bpf_iograph_run_action(struct bpf_map *map, const u8 *buf,
				       u32 buf__sz, u32 entry_id);
__bpf_kfunc u32 bpf_iograph_run_action_idx(struct bpf_map *map, const u8 *buf,
					   u32 buf__sz, u32 entry_idx);
__bpf_kfunc u32 bpf_iograph_step(struct bpf_map *map, u32 state, u32 sym);

#endif /* _BPF_IOGRAPH_INTERNAL_H */
