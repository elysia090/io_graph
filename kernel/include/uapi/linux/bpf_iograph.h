/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_BPF_IOGRAPH_H
#define _UAPI_LINUX_BPF_IOGRAPH_H

#include <linux/types.h>

#define IOG_MAGIC		0x494f4752u
#define IOG_VERSION		1u
#define IOG_NO_STATE		(~0U)
#define IOG_NODE_F_FINAL_ACTION	(1u << 0)
#define IOG_NODE_FLAG_MASK	IOG_NODE_F_FINAL_ACTION
#define BPF_F_IOGRAPH_ACTION_ONLY (1u << 31)

struct iog_blob_hdr {
	__u32 magic;
	__u16 version;
	__u16 flags;
	__u32 node_cnt;
	__u32 edge_cnt;
	__u32 entry_cnt;
	__u32 accept_cnt;
	__u32 alphabet_size;
	__u32 initial_state;
	__u32 nodes_off;
	__u32 edges_off;
	__u32 entries_off;
	__u32 accepts_off;
	__u32 total_size;
	__u32 max_input_len;
	__u32 reserved;
};

struct iog_node {
	__u32 edge_start;
	__u16 edge_cnt;
	__u16 flags;
	/* Consuming else transition on explicit edge miss, or IOG_NO_STATE. */
	__u32 default_dst;
	__u32 accept_id;
};

struct iog_edge {
	__u32 sym_lo;
	__u32 sym_hi;
	__u32 dst;
};

struct iog_entry {
	__u32 id;
	__u32 state;
};

struct iog_accept {
	__u32 id;
	__u32 code;
};

struct bpf_iograph_run_result {
	__u32 final_state;
	__u32 action_code;
};

#endif /* _UAPI_LINUX_BPF_IOGRAPH_H */
