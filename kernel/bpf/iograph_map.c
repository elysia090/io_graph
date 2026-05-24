// SPDX-License-Identifier: GPL-2.0-only
/*
 * BPF_MAP_TYPE_IOGRAPH prototype.
 *
 * This file is intentionally written as Linux kernel code, not as a benchmark
 * shim. It follows the map-object pattern used by kernel/bpf/lpm_trie.c and
 * the unsupported key/value operations pattern used by kernel/bpf/ringbuf.c.
 *
 * Integration still required in a Linux tree:
 *   - add BPF_MAP_TYPE_IOGRAPH to include/uapi/linux/bpf.h;
 *   - wire iograph_map_ops through include/linux/bpf_types.h;
 *   - add this object to kernel/bpf/Makefile;
 *   - register kfuncs for the intended BPF program types.
 */

#include <linux/bpf.h>
#include <linux/btf_ids.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "iograph_internal.h"

#define IOG_CREATE_FLAG_MASK	BPF_F_IOGRAPH_ACTION_ONLY

static bool iog_u32_array_fits(u32 total, u32 off, u32 cnt, size_t elem_sz)
{
	size_t bytes;

	if (check_mul_overflow((size_t)cnt, elem_sz, &bytes))
		return false;
	if (off > total)
		return false;
	if (bytes > total - off)
		return false;

	return true;
}

static bool iog_layout_end(u32 off, u32 cnt, size_t elem_sz, u32 *end)
{
	size_t bytes, next;

	if (check_mul_overflow((size_t)cnt, elem_sz, &bytes))
		return false;
	if (check_add_overflow((size_t)off, bytes, &next))
		return false;
	if (next > U32_MAX)
		return false;

	*end = next;
	return true;
}

static int iog_verify_blob(const void *blob, u32 value_size, u32 *blob_len)
{
	const struct iog_blob_hdr *hdr = blob;
	const u8 *base = blob;
	const struct iog_node *nodes;
	const struct iog_edge *edges;
	const struct iog_entry *entries;
	const struct iog_accept *accepts;
	u32 expected_off;
	u32 i;

	if (!blob || value_size < sizeof(*hdr) || !blob_len)
		return -EINVAL;
	if (hdr->magic != IOG_MAGIC || hdr->version != IOG_VERSION)
		return -EINVAL;
	if (hdr->flags || hdr->reserved)
		return -EINVAL;
	if (hdr->total_size < sizeof(*hdr) || hdr->total_size > value_size)
		return -EINVAL;
	if (hdr->total_size > BPF_IOGRAPH_MAX_BLOB_SIZE)
		return -E2BIG;
	if (!hdr->node_cnt || hdr->initial_state >= hdr->node_cnt)
		return -EINVAL;
	if (hdr->node_cnt > BPF_IOGRAPH_MAX_NODES ||
	    hdr->edge_cnt > BPF_IOGRAPH_MAX_EDGES ||
	    hdr->entry_cnt > BPF_IOGRAPH_MAX_ENTRIES ||
	    hdr->accept_cnt > BPF_IOGRAPH_MAX_ACCEPTS ||
	    hdr->max_input_len > BPF_IOGRAPH_MAX_INPUT_LEN)
		return -E2BIG;
	if (!hdr->alphabet_size || hdr->alphabet_size > 256)
		return -EINVAL;
	if ((hdr->nodes_off | hdr->edges_off |
	     hdr->entries_off | hdr->accepts_off) & 3)
		return -EINVAL;
	if (!iog_u32_array_fits(hdr->total_size, hdr->nodes_off, hdr->node_cnt,
				sizeof(struct iog_node)) ||
	    !iog_u32_array_fits(hdr->total_size, hdr->edges_off, hdr->edge_cnt,
				sizeof(struct iog_edge)) ||
	    !iog_u32_array_fits(hdr->total_size, hdr->entries_off, hdr->entry_cnt,
				sizeof(struct iog_entry)) ||
	    !iog_u32_array_fits(hdr->total_size, hdr->accepts_off, hdr->accept_cnt,
				sizeof(struct iog_accept)))
		return -EINVAL;
	if (hdr->nodes_off != sizeof(*hdr) ||
	    !iog_layout_end(hdr->nodes_off, hdr->node_cnt,
			    sizeof(struct iog_node), &expected_off) ||
	    hdr->edges_off != expected_off ||
	    !iog_layout_end(hdr->edges_off, hdr->edge_cnt,
			    sizeof(struct iog_edge), &expected_off) ||
	    hdr->entries_off != expected_off ||
	    !iog_layout_end(hdr->entries_off, hdr->entry_cnt,
			    sizeof(struct iog_entry), &expected_off) ||
	    hdr->accepts_off != expected_off ||
	    !iog_layout_end(hdr->accepts_off, hdr->accept_cnt,
			    sizeof(struct iog_accept), &expected_off) ||
	    hdr->total_size != expected_off)
		return -EINVAL;

	nodes = (const void *)(base + hdr->nodes_off);
	edges = (const void *)(base + hdr->edges_off);
	entries = (const void *)(base + hdr->entries_off);
	accepts = (const void *)(base + hdr->accepts_off);

	for (i = 0; i < hdr->accept_cnt; i++) {
		if (accepts[i].id != i)
			return -EINVAL;
	}

	for (i = 0; i < hdr->entry_cnt; i++) {
		if (entries[i].state >= hdr->node_cnt)
			return -EINVAL;
	}

	for (i = 0; i < hdr->node_cnt; i++) {
		const struct iog_node *node = &nodes[i];
		u32 prev_hi = 0;
		bool have_prev = false;
		u32 j;

		if (node->flags & ~IOG_NODE_FLAG_MASK)
			return -EINVAL;
		if ((node->flags & IOG_NODE_F_FINAL_ACTION) &&
		    !node->accept_id)
			return -EINVAL;
		if (node->edge_start > hdr->edge_cnt ||
		    node->edge_cnt > hdr->edge_cnt - node->edge_start)
			return -EINVAL;
		if (node->default_dst != IOG_NO_STATE &&
		    node->default_dst >= hdr->node_cnt)
			return -EINVAL;
		if (node->accept_id && node->accept_id >= hdr->accept_cnt)
			return -EINVAL;

		for (j = 0; j < node->edge_cnt; j++) {
			const struct iog_edge *edge =
				&edges[node->edge_start + j];

			if (edge->sym_lo > edge->sym_hi ||
			    edge->sym_hi >= hdr->alphabet_size)
				return -EINVAL;
			if (edge->dst >= hdr->node_cnt)
				return -EINVAL;
			if (have_prev && edge->sym_lo <= prev_hi)
				return -EINVAL;
			prev_hi = edge->sym_hi;
			have_prev = true;
		}
	}

	*blob_len = hdr->total_size;
	return 0;
}

static void iograph_graph_inline_accept_codes(struct bpf_iograph_graph *graph)
{
	struct iog_node *nodes = (void *)graph->nodes;
	u32 i;

	for (i = 0; i < graph->hdr->node_cnt; i++) {
		u32 accept_id = nodes[i].accept_id;

		nodes[i].accept_id = accept_id ? graph->accepts[accept_id].code :
					     0;
	}
}

static bool iograph_byte_edge(const struct iog_edge *edge)
{
	return edge->sym_lo == edge->sym_hi && edge->sym_hi <= U8_MAX;
}

static void iograph_compact_keep_entries(const struct bpf_iograph_graph *graph,
					 u8 *keep)
{
	u32 i;

	keep[graph->hdr->initial_state] = 1;
	for (i = 0; i < graph->hdr->entry_cnt; i++)
		keep[graph->entries[i].state] = 1;
}

static bool iograph_compact_final_action_leaf(const struct bpf_iograph_graph *graph,
					      u32 state)
{
	const struct iog_node *node = &graph->nodes[state];

	return node->accept_id &&
	       (node->flags & IOG_NODE_F_FINAL_ACTION) &&
	       node->edge_cnt == 0 && node->default_dst == IOG_NO_STATE;
}

static int iograph_compact_build_keep(const struct bpf_iograph_graph *graph,
				      u8 *keep, u32 *incoming)
{
	u32 i;

	iograph_compact_keep_entries(graph, keep);
	for (i = 0; i < graph->hdr->node_cnt; i++) {
		const struct iog_node *node = &graph->nodes[i];
		u32 j;

		if (iograph_compact_final_action_leaf(graph, i)) {
			/* Incoming edges can carry this terminal action. */
		} else if (node->accept_id || node->flags ||
			   node->default_dst != IOG_NO_STATE ||
			   node->edge_cnt != 1) {
			keep[i] = 1;
		} else if (!iograph_byte_edge(&graph->edges[node->edge_start])) {
			keep[i] = 1;
		}
		if (node->default_dst != IOG_NO_STATE) {
			incoming[node->default_dst]++;
			keep[node->default_dst] = 1;
		}

		for (j = 0; j < node->edge_cnt; j++) {
			const struct iog_edge *edge =
				&graph->edges[node->edge_start + j];

			incoming[edge->dst]++;
			if (!iograph_byte_edge(edge) &&
			    !iograph_compact_final_action_leaf(graph, edge->dst))
				keep[edge->dst] = 1;
		}
	}

	for (i = 0; i < graph->hdr->node_cnt; i++) {
		if (!iograph_compact_final_action_leaf(graph, i) &&
		    incoming[i] != 1)
			keep[i] = 1;
	}

	return 0;
}

static u32 iograph_compact_assign_nodes(const struct bpf_iograph_graph *graph,
					const u8 *keep, u32 *orig_to_compact)
{
	u32 i, nr = 0;

	for (i = 0; i < graph->hdr->node_cnt; i++) {
		if (keep[i])
			orig_to_compact[i] = nr++;
		else
			orig_to_compact[i] = IOG_NO_STATE;
	}

	return nr;
}

struct iograph_compact_chain_meta {
	u32 endpoint;
	u32 len;
};

static int
iograph_compact_chain_meta_get(const struct bpf_iograph_graph *graph,
			       const u8 *keep,
			       struct iograph_compact_chain_meta *meta,
			       u32 dst, u32 *len, u32 *endpoint)
{
	u32 state = dst, nr = 1;

	if (meta[dst].len) {
		*len = meta[dst].len;
		*endpoint = meta[dst].endpoint;
		return 0;
	}

	while (!keep[state] &&
	       !iograph_compact_final_action_leaf(graph, state)) {
		const struct iog_node *node = &graph->nodes[state];
		const struct iog_edge *edge = &graph->edges[node->edge_start];

		if (nr > graph->hdr->node_cnt)
			return -EINVAL;
		state = edge->dst;
		nr++;
	}

	meta[dst].len = nr;
	meta[dst].endpoint = state;
	*len = nr;
	*endpoint = state;
	return 0;
}

static void iograph_compact_copy_literal_tail(const struct bpf_iograph_graph *graph,
					      const u8 *keep,
					      const struct iog_edge *first,
					      u8 *dst)
{
	const struct iog_edge *edge = first;
	u32 state = edge->dst;

	while (!keep[state] &&
	       !iograph_compact_final_action_leaf(graph, state)) {
		const struct iog_node *node = &graph->nodes[state];

		edge = &graph->edges[node->edge_start];
		*dst++ = (u8)edge->sym_lo;
		state = edge->dst;
	}
}

static int iograph_compact_count_storage(const struct bpf_iograph_graph *graph,
					 const u8 *keep,
					 struct iograph_compact_chain_meta *meta,
					 u32 *edge_cnt,
					 u32 *lit_len, u32 *dispatch_cnt)
{
	u32 i, edges = 0, lits = 0, dispatch = 0;

	for (i = 0; i < graph->hdr->node_cnt; i++) {
		const struct iog_node *node = &graph->nodes[i];
		u32 j;

		if (!keep[i])
			continue;
		if (node->edge_cnt >= BPF_IOGRAPH_DISPATCH256_THRESHOLD) {
			if (dispatch == U32_MAX)
				return -E2BIG;
			dispatch++;
		}

		for (j = 0; j < node->edge_cnt; j++) {
			const struct iog_edge *edge =
				&graph->edges[node->edge_start + j];
			u32 clen = 1;
			u32 endpoint;
			int ret;

			edges++;
			if (iograph_byte_edge(edge)) {
				ret = iograph_compact_chain_meta_get(graph,
								     keep, meta,
								     edge->dst,
								     &clen,
								     &endpoint);
				if (ret)
					return ret;
			}
			if (clen > 1) {
				u32 next_lits, tail_len = clen - 1;

				if (check_add_overflow(lits, tail_len,
						       &next_lits))
					return -E2BIG;
				lits = next_lits;
			}
		}
	}

	*edge_cnt = edges;
	*lit_len = lits;
	*dispatch_cnt = dispatch;
	return 0;
}

static void iograph_compact_free(struct bpf_iograph_graph *graph)
{
	if (!graph)
		return;

	kvfree(graph->compact_data);
	graph->compact_data = NULL;
	graph->compact_entries = NULL;
	graph->compact_nodes = NULL;
	graph->compact_edges = NULL;
	graph->compact_dispatch = NULL;
	graph->compact_lits = NULL;
	graph->compact_mem_bytes = 0;
}

static bool iograph_compact_add_section(size_t *off, size_t align,
					u32 cnt, size_t elem_sz,
					size_t *section_off)
{
	size_t aligned = ALIGN(*off, align);
	size_t bytes, next;

	if (aligned < *off)
		return false;
	if (check_mul_overflow((size_t)cnt, elem_sz, &bytes))
		return false;
	if (check_add_overflow(aligned, bytes, &next))
		return false;

	*section_off = aligned;
	*off = next;
	return true;
}

static int iograph_compact_alloc_arrays(struct bpf_iograph_graph *graph,
					u32 node_cnt, u32 edge_cnt,
					u32 lit_len, u32 dispatch_cnt)
{
	u32 entry_cnt = graph->hdr->entry_cnt;
	size_t entries_off = 0, nodes_off = 0, edges_off = 0;
	size_t dispatch_off = 0, lits_off = 0;
	u32 dispatch_slots = 0;
	size_t total = 0;
	u8 *data;

	if (check_mul_overflow(dispatch_cnt, BPF_IOGRAPH_DISPATCH256_SIZE,
			       &dispatch_slots))
		return -E2BIG;

	if (!iograph_compact_add_section(&total,
					 __alignof__(struct iog_entry),
					 entry_cnt,
					 sizeof(*graph->compact_entries),
					 &entries_off) ||
	    !iograph_compact_add_section(&total,
					 __alignof__(struct bpf_iograph_cnode),
					 node_cnt,
					 sizeof(*graph->compact_nodes),
					 &nodes_off) ||
	    !iograph_compact_add_section(&total,
					 __alignof__(struct bpf_iograph_cedge),
					 edge_cnt,
					 sizeof(*graph->compact_edges),
					 &edges_off) ||
	    !iograph_compact_add_section(&total,
					 __alignof__(u16),
					 dispatch_slots,
					 sizeof(*graph->compact_dispatch),
					 &dispatch_off) ||
	    !iograph_compact_add_section(&total, __alignof__(u8), lit_len,
					 sizeof(*graph->compact_lits),
					 &lits_off))
		return -E2BIG;
	if (total > BPF_IOGRAPH_MAX_COMPACT_SIZE)
		return -E2BIG;

	data = kvzalloc(total, GFP_KERNEL_ACCOUNT);
	if (!data)
		return -ENOMEM;

	graph->compact_data = data;
	graph->compact_entries = entry_cnt ?
				 (struct iog_entry *)(data + entries_off) :
				 NULL;
	graph->compact_nodes = (struct bpf_iograph_cnode *)(data + nodes_off);
	graph->compact_edges = edge_cnt ?
			       (struct bpf_iograph_cedge *)(data + edges_off) :
			       NULL;
	graph->compact_dispatch = dispatch_slots ?
				  (u16 *)(data + dispatch_off) : NULL;
	graph->compact_lits = lit_len ? data + lits_off : NULL;
	graph->compact_entry_cnt = entry_cnt;
	graph->compact_node_cnt = node_cnt;
	graph->compact_edge_cnt = edge_cnt;
	graph->compact_lit_len = lit_len;
	graph->compact_dispatch_cnt = dispatch_cnt;
	graph->compact_mem_bytes = total;
	return 0;
}

static void iograph_compact_build_dispatch(struct bpf_iograph_graph *graph,
					   struct bpf_iograph_cnode *cnode)
{
	u16 *dispatch = &graph->compact_dispatch[cnode->dispatch_start - 1];
	u32 i;

	for (i = 0; i < BPF_IOGRAPH_DISPATCH256_SIZE; i++)
		dispatch[i] = U16_MAX;

	for (i = 0; i < cnode->edge_cnt; i++) {
		const struct bpf_iograph_cedge *edge =
			&graph->compact_edges[cnode->edge_start + i];
		u32 sym;

		for (sym = edge->sym_lo; sym <= edge->sym_hi; sym++)
			dispatch[sym] = (u16)i;
	}
}

static int iograph_compact_build_edges(struct bpf_iograph_graph *graph,
				       const u8 *keep,
				       const u32 *orig_to_compact,
				       struct iograph_compact_chain_meta *meta)
{
	u32 i, edge_pos = 0, lit_pos = 0, dispatch_pos = 0;

	for (i = 0; i < graph->hdr->entry_cnt; i++) {
		graph->compact_entries[i] = graph->entries[i];
		graph->compact_entries[i].state =
			orig_to_compact[graph->entries[i].state];
	}

	for (i = 0; i < graph->hdr->node_cnt; i++) {
		const struct iog_node *node = &graph->nodes[i];
		struct bpf_iograph_cnode *cnode;
		u32 cstate, j;

		if (!keep[i])
			continue;

		cstate = orig_to_compact[i];
		cnode = &graph->compact_nodes[cstate];
		cnode->edge_start = edge_pos;
		cnode->edge_cnt = node->edge_cnt;
		cnode->flags = node->flags;
		cnode->action_code = node->accept_id;
		cnode->default_dst = node->default_dst == IOG_NO_STATE ?
				     IOG_NO_STATE :
				     orig_to_compact[node->default_dst];
		if (node->edge_cnt >= BPF_IOGRAPH_DISPATCH256_THRESHOLD) {
			cnode->dispatch_start = dispatch_pos + 1;
			dispatch_pos += BPF_IOGRAPH_DISPATCH256_SIZE;
		}

		for (j = 0; j < node->edge_cnt; j++) {
			const struct iog_edge *edge =
				&graph->edges[node->edge_start + j];
			struct bpf_iograph_cedge *cedge =
				&graph->compact_edges[edge_pos++];
			u32 clen = 1;
			u32 endpoint = edge->dst;
			int ret;

			cedge->sym_lo = (u8)edge->sym_lo;
			cedge->sym_hi = (u8)edge->sym_hi;
			if (iograph_byte_edge(edge)) {
				ret = iograph_compact_chain_meta_get(graph,
								     keep, meta,
								     edge->dst,
								     &clen,
								     &endpoint);
				if (ret)
					return ret;
			}
			if (iograph_compact_final_action_leaf(graph, endpoint)) {
				cedge->flags |= BPF_IOGRAPH_CEDGE_FINAL_ACTION;
				cedge->dst = graph->nodes[endpoint].accept_id;
			} else {
				cedge->dst = orig_to_compact[endpoint];
			}
			if (clen > 1) {
				u32 tail_len = clen - 1;

				cedge->flags |= BPF_IOGRAPH_CEDGE_LITERAL;
				cedge->lit_off = lit_pos;
				cedge->lit_len = tail_len;
				graph->compact_literal_edge_cnt++;
				if (tail_len > graph->compact_max_literal_len)
					graph->compact_max_literal_len = tail_len;
				iograph_compact_copy_literal_tail(graph, keep, edge,
								  &graph->compact_lits[lit_pos]);
				lit_pos += tail_len;
			}
		}
		if (cnode->dispatch_start)
			iograph_compact_build_dispatch(graph, cnode);
	}

	if (graph->single_entry)
		graph->compact_single_entry_state =
			graph->compact_entries[0].state;
	return 0;
}

static int iograph_graph_build_compact(struct bpf_iograph_graph *graph)
{
	u8 *keep = NULL;
	u32 *incoming = NULL, *orig_to_compact = NULL;
	struct iograph_compact_chain_meta *meta = NULL;
	u32 node_cnt, edge_cnt = 0, lit_len = 0, dispatch_cnt = 0;
	int ret;

	keep = kvcalloc(graph->hdr->node_cnt, sizeof(*keep), GFP_KERNEL_ACCOUNT);
	incoming = kvcalloc(graph->hdr->node_cnt, sizeof(*incoming),
			    GFP_KERNEL_ACCOUNT);
	orig_to_compact = kvmalloc_array(graph->hdr->node_cnt,
					 sizeof(*orig_to_compact),
					 GFP_KERNEL_ACCOUNT);
	meta = kvcalloc(graph->hdr->node_cnt, sizeof(*meta), GFP_KERNEL_ACCOUNT);
	if (!keep || !incoming || !orig_to_compact || !meta) {
		ret = -ENOMEM;
		goto out;
	}

	ret = iograph_compact_build_keep(graph, keep, incoming);
	if (ret)
		goto out;
	node_cnt = iograph_compact_assign_nodes(graph, keep, orig_to_compact);
	ret = iograph_compact_count_storage(graph, keep, meta, &edge_cnt,
					    &lit_len,
					    &dispatch_cnt);
	if (ret)
		goto out;
	ret = iograph_compact_alloc_arrays(graph, node_cnt, edge_cnt, lit_len,
					   dispatch_cnt);
	if (ret)
		goto out;
	ret = iograph_compact_build_edges(graph, keep, orig_to_compact, meta);

out:
	if (ret)
		iograph_compact_free(graph);
	kvfree(meta);
	kvfree(orig_to_compact);
	kvfree(incoming);
	kvfree(keep);
	return ret;
}

static void iograph_graph_free(struct bpf_iograph_graph *graph)
{
	if (!graph)
		return;

	iograph_compact_free(graph);
	kvfree(graph->blob);
	kvfree(graph);
}

static void iograph_graph_drop_blob(struct bpf_iograph_graph *graph)
{
	kvfree(graph->blob);
	graph->blob = NULL;
	graph->blob_len = 0;
	graph->hdr = NULL;
	graph->nodes = NULL;
	graph->edges = NULL;
	graph->entries = NULL;
	graph->accepts = NULL;
	graph->node_cnt = 0;
	graph->entry_cnt = 0;
}

static struct bpf_iograph_graph *iograph_graph_alloc(const void *value,
						     u32 value_size,
						     int numa_node,
						     bool action_only)
{
	struct bpf_iograph_graph *graph;
	u32 blob_len;
	int ret;

	ret = iog_verify_blob(value, value_size, &blob_len);
	if (ret)
		return ERR_PTR(ret);

	graph = kvzalloc_node(sizeof(*graph), GFP_KERNEL_ACCOUNT, numa_node);
	if (!graph)
		return ERR_PTR(-ENOMEM);

	graph->blob = kvmalloc(blob_len, GFP_KERNEL_ACCOUNT);
	if (!graph->blob) {
		kvfree(graph);
		return ERR_PTR(-ENOMEM);
	}

	memcpy(graph->blob, value, blob_len);
	graph->blob_len = blob_len;
	graph->hdr = (const void *)graph->blob;
	graph->nodes = (const void *)(graph->blob + graph->hdr->nodes_off);
	graph->edges = (const void *)(graph->blob + graph->hdr->edges_off);
	graph->entries = (const void *)(graph->blob + graph->hdr->entries_off);
	graph->accepts = (const void *)(graph->blob + graph->hdr->accepts_off);
	graph->node_cnt = graph->hdr->node_cnt;
	graph->entry_cnt = graph->hdr->entry_cnt;
	graph->max_input_len = graph->hdr->max_input_len;
	iograph_graph_inline_accept_codes(graph);
	if (graph->hdr->entry_cnt == 1) {
		graph->single_entry_id = graph->entries[0].id;
		graph->single_entry_state = graph->entries[0].state;
		graph->single_entry = true;
	}
	ret = iograph_graph_build_compact(graph);
	if (ret) {
		iograph_graph_free(graph);
		return ERR_PTR(ret);
	}
	if (action_only)
		iograph_graph_drop_blob(graph);
	return graph;
}

static void iograph_graph_free_rcu(struct rcu_head *rcu)
{
	struct bpf_iograph_graph *graph;

	graph = container_of(rcu, struct bpf_iograph_graph, rcu);
	iograph_graph_free(graph);
}

static struct bpf_map *iograph_map_alloc(union bpf_attr *attr)
{
	struct bpf_iograph_map *imap;

	if (attr->map_flags & ~IOG_CREATE_FLAG_MASK)
		return ERR_PTR(-EINVAL);
	if (attr->key_size != sizeof(u32) || !attr->value_size ||
	    attr->max_entries != 1)
		return ERR_PTR(-EINVAL);
	if (attr->value_size < sizeof(struct iog_blob_hdr))
		return ERR_PTR(-EINVAL);
	if (attr->value_size > BPF_IOGRAPH_MAX_BLOB_SIZE)
		return ERR_PTR(-E2BIG);

	imap = bpf_map_area_alloc(sizeof(*imap), NUMA_NO_NODE);
	if (!imap)
		return ERR_PTR(-ENOMEM);

	bpf_map_init_from_attr(&imap->map, attr);
	rcu_assign_pointer(imap->graph, NULL);
	mutex_init(&imap->update_lock);
	imap->update_seq = 0;
	return &imap->map;
}

static void iograph_map_free(struct bpf_map *map)
{
	struct bpf_iograph_map *imap;
	struct bpf_iograph_graph *graph;

	imap = container_of(map, struct bpf_iograph_map, map);
	graph = rcu_dereference_protected(imap->graph, true);
	RCU_INIT_POINTER(imap->graph, NULL);
	if (graph) {
		synchronize_rcu();
		iograph_graph_free(graph);
	}
	bpf_map_area_free(imap);
}

static void *iograph_map_lookup_elem(struct bpf_map *map, void *key)
{
	(void)map;
	(void)key;
	return ERR_PTR(-ENOTSUPP);
}

static long iograph_map_update_elem(struct bpf_map *map, void *key,
				    void *value, u64 flags)
{
	struct bpf_iograph_map *imap;
	struct bpf_iograph_graph *new_graph, *old_graph;
	u32 idx;
	long ret = 0;

	if (flags != BPF_ANY && flags != BPF_NOEXIST && flags != BPF_EXIST)
		return -EINVAL;

	if (!key)
		return -EINVAL;
	idx = *(u32 *)key;
	if (idx)
		return -EINVAL;

	imap = container_of(map, struct bpf_iograph_map, map);
	new_graph = iograph_graph_alloc(value, map->value_size, map->numa_node,
					map->map_flags & BPF_F_IOGRAPH_ACTION_ONLY);
	if (IS_ERR(new_graph))
		return PTR_ERR(new_graph);

	mutex_lock(&imap->update_lock);
	old_graph = rcu_dereference_protected(imap->graph,
					      lockdep_is_held(&imap->update_lock));
	if (flags == BPF_NOEXIST && old_graph) {
		ret = -EEXIST;
		goto out_unlock;
	}
	if (flags == BPF_EXIST && !old_graph) {
		ret = -ENOENT;
		goto out_unlock;
	}

	rcu_assign_pointer(imap->graph, new_graph);
	imap->update_seq++;
	new_graph = NULL;

out_unlock:
	mutex_unlock(&imap->update_lock);
	if (old_graph && !ret)
		call_rcu(&old_graph->rcu, iograph_graph_free_rcu);
	iograph_graph_free(new_graph);
	return ret;
}

static long iograph_map_delete_elem(struct bpf_map *map, void *key)
{
	struct bpf_iograph_map *imap;
	struct bpf_iograph_graph *old_graph;
	u32 idx;

	if (!key)
		return -EINVAL;
	idx = *(u32 *)key;

	if (idx)
		return -EINVAL;

	imap = container_of(map, struct bpf_iograph_map, map);
	mutex_lock(&imap->update_lock);
	old_graph = rcu_dereference_protected(imap->graph,
					      lockdep_is_held(&imap->update_lock));
	if (!old_graph) {
		mutex_unlock(&imap->update_lock);
		return -ENOENT;
	}
	RCU_INIT_POINTER(imap->graph, NULL);
	imap->update_seq++;
	mutex_unlock(&imap->update_lock);

	call_rcu(&old_graph->rcu, iograph_graph_free_rcu);
	return 0;
}

static int iograph_map_get_next_key(struct bpf_map *map, void *key,
				    void *next_key)
{
	(void)map;
	(void)key;
	(void)next_key;
	return -ENOTSUPP;
}

static u64 iograph_map_mem_usage(const struct bpf_map *map)
{
	const struct bpf_iograph_map *imap;
	const struct bpf_iograph_graph *graph;
	u64 usage = sizeof(*imap);

	imap = container_of(map, struct bpf_iograph_map, map);
	rcu_read_lock();
	graph = rcu_dereference(imap->graph);
	if (graph) {
		usage += sizeof(*graph);
		usage += graph->blob_len;
		usage += graph->compact_mem_bytes;
	}
	rcu_read_unlock();

	return usage;
}

BTF_ID_LIST_SINGLE(iograph_map_btf_ids, struct, bpf_iograph_map)

const struct bpf_map_ops iograph_map_ops = {
	.map_meta_equal = bpf_map_meta_equal,
	.map_alloc = iograph_map_alloc,
	.map_free = iograph_map_free,
	.map_lookup_elem = iograph_map_lookup_elem,
	.map_update_elem = iograph_map_update_elem,
	.map_delete_elem = iograph_map_delete_elem,
	.map_get_next_key = iograph_map_get_next_key,
	.map_mem_usage = iograph_map_mem_usage,
	.map_btf_id = &iograph_map_btf_ids[0],
};
