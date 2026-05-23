#ifndef IOG_COMPACT_H
#define IOG_COMPACT_H

#include <iog/graph.h>

#define IOG_CEDGE_LITERAL	(1u << 0)
#define IOG_CEDGE_FINAL_ACTION	(1u << 1)

struct iog_cnode {
	u32 edge_start;
	u16 edge_cnt;
	u16 flags;
	u32 default_dst;
	u32 action_code;
};

struct iog_cedge {
	/*
	 * dst is a compact node id unless IOG_CEDGE_FINAL_ACTION is set, in
	 * which case it is the final action code returned after the edge
	 * matches.
	 */
	u32 dst;
	u32 lit_off;
	/* Tail bytes after the first dispatch byte. */
	u32 lit_len;
	u8 sym_lo;
	u8 sym_hi;
	u8 flags;
};

struct iog_cgraph {
	u32 node_cnt;
	u32 edge_cnt;
	u32 lit_len;
	u32 max_input_len;
	u32 single_entry_id;
	u32 single_entry_state;
	bool single_entry;
	struct iog_entry *entries;
	u32 entry_cnt;
	struct iog_cnode *nodes;
	struct iog_cedge *edges;
	u8 *lits;
};

struct iog_cgraph_stats {
	u32 nodes;
	u32 edges;
	u32 literal_edges;
	u32 literal_bytes;
	u32 max_literal_len;
	u32 max_fanout;
	u32 max_depth;
	bool depth_complete;
	u64 mem_bytes;
};

int iog_cgraph_new(const struct iog_graph *graph,
		   struct iog_cgraph **out);
void iog_cgraph_free(struct iog_cgraph *cg);
u64 iog_cgraph_mem_bytes(const struct iog_cgraph *cg);
u32 iog_cgraph_run_action_entry(const struct iog_cgraph *cg, const u8 *buf,
				u32 len, u32 entry_id);
u32 iog_cgraph_run_action(const struct iog_cgraph *cg, const u8 *buf,
			  u32 len);
u32 iog_cgraph_count_transitions_entry(const struct iog_cgraph *cg,
				       const u8 *buf, u32 len, u32 entry_id);
u32 iog_cgraph_count_transitions(const struct iog_cgraph *cg, const u8 *buf,
				 u32 len);
int iog_cgraph_stats(const struct iog_cgraph *cg,
		     struct iog_cgraph_stats *stats);

#endif
