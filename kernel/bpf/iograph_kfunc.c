// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bpf.h>
#include <linux/btf_ids.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/rcupdate_trace.h>
#include <linux/string.h>

#include "iograph_internal.h"

static __always_inline u32
iograph_node_accept_code(const struct bpf_iograph_graph *graph,
			 const struct iog_node *node)
{
	(void)graph;
	return node->accept_id;
}

static __always_inline u32
iograph_node_next_state(const struct iog_edge *all_edges,
			const struct iog_node *node, u8 sym)
{
	const struct iog_edge *edges = &all_edges[node->edge_start];
	u32 lo = 0, hi = node->edge_cnt;
	u32 i;

	if (likely(node->edge_cnt == 1)) {
		const struct iog_edge *edge = edges;

		if (sym < edge->sym_lo)
			return node->default_dst;
		if (sym <= edge->sym_hi)
			return edge->dst;
		return node->default_dst;
	}

	if (likely(node->edge_cnt <= 4)) {
		for (i = 0; i < node->edge_cnt; i++) {
			const struct iog_edge *edge = &edges[i];

			if (sym < edge->sym_lo)
				break;
			if (sym <= edge->sym_hi)
				return edge->dst;
		}
		return node->default_dst;
	}

	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;
		const struct iog_edge *edge = &edges[mid];

		if (sym < edge->sym_lo)
			hi = mid;
		else if (sym > edge->sym_hi)
			lo = mid + 1;
		else
			return edge->dst;
	}

	return node->default_dst;
}

static __always_inline u32
iograph_step_state(const struct bpf_iograph_graph *graph, u32 state, u8 sym)
{
	return iograph_node_next_state(graph->edges, &graph->nodes[state], sym);
}

static __always_inline struct bpf_iograph_graph *
iograph_active_graph(const struct bpf_iograph_map *imap)
{
	return rcu_dereference_check(imap->graph,
				     rcu_read_lock_held() ||
				     rcu_read_lock_trace_held() ||
				     rcu_read_lock_bh_held());
}

static __always_inline int
iograph_entry_state(const struct bpf_iograph_graph *graph, u32 entry_id,
		    u32 *state)
{
	u32 i;

	if (likely(graph->single_entry &&
		   graph->single_entry_id == entry_id)) {
		*state = graph->single_entry_state;
		return 0;
	}

	for (i = 0; i < graph->hdr->entry_cnt; i++) {
		if (graph->entries[i].id == entry_id) {
			*state = graph->entries[i].state;
			return 0;
		}
	}

	return -ENOENT;
}

static u32 iograph_walk_result(const struct bpf_iograph_graph *graph,
			       const u8 *buf, u32 len, u32 state,
			       u32 *final_state)
{
	const struct iog_edge *edges = graph->edges;
	const struct iog_node *nodes = graph->nodes;
	const struct iog_node *node = &nodes[state];
	u32 action = iograph_node_accept_code(graph, node);
	u32 i;

	for (i = 0; i < len; i++) {
		u32 next = iograph_node_next_state(edges, node, buf[i]);
		u32 accept_id, state_action;

		if (next == IOG_NO_STATE)
			break;
		state = next;
		node = &nodes[state];
		accept_id = node->accept_id;
		if (likely(!accept_id))
			continue;
		state_action = accept_id;
		if (state_action)
			action = state_action;
	}
	if (final_state)
		*final_state = state;
	return action;
}

static __always_inline int
iograph_compact_entry_state(const struct bpf_iograph_graph *graph,
			    u32 entry_id, u32 *state)
{
	u32 i;

	if (likely(graph->single_entry &&
		   graph->single_entry_id == entry_id)) {
		*state = graph->compact_single_entry_state;
		return 0;
	}

	for (i = 0; i < graph->hdr->entry_cnt; i++) {
		if (graph->compact_entries[i].id == entry_id) {
			*state = graph->compact_entries[i].state;
			return 0;
		}
	}

	return -ENOENT;
}

static __always_inline const struct bpf_iograph_cedge *
iograph_cnode_find_edge(const struct bpf_iograph_graph *graph,
			const struct bpf_iograph_cnode *node, u8 sym)
{
	const struct bpf_iograph_cedge *edges;
	u32 lo = 0, hi = node->edge_cnt;
	u32 i;

	if (!node->edge_cnt)
		return NULL;

	edges = &graph->compact_edges[node->edge_start];
	if (likely(node->edge_cnt == 1)) {
		const struct bpf_iograph_cedge *edge = edges;

		if (sym < edge->sym_lo || sym > edge->sym_hi)
			return NULL;
		return edge;
	}

	if (likely(node->edge_cnt <= 4)) {
		for (i = 0; i < node->edge_cnt; i++) {
			const struct bpf_iograph_cedge *edge = &edges[i];

			if (sym < edge->sym_lo)
				break;
			if (sym <= edge->sym_hi)
				return edge;
		}
		return NULL;
	}

	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;
		const struct bpf_iograph_cedge *edge = &edges[mid];

		if (sym < edge->sym_lo)
			hi = mid;
		else if (sym > edge->sym_hi)
			lo = mid + 1;
		else
			return edge;
	}

	return NULL;
}

static u32 iograph_walk_action_compact(const struct bpf_iograph_graph *graph,
				       const u8 *buf, u32 len, u32 state)
{
	const struct bpf_iograph_cnode *node = &graph->compact_nodes[state];
	u32 action = node->action_code;
	u32 i = 0;

	if (action && (node->flags & IOG_NODE_F_FINAL_ACTION))
		return action;

	while (i < len) {
		const struct bpf_iograph_cedge *edge =
			iograph_cnode_find_edge(graph, node, buf[i]);

		if (!edge) {
			if (node->default_dst == IOG_NO_STATE)
				break;
			node = &graph->compact_nodes[node->default_dst];
			i++;
		} else {
			i++;
			if (edge->flags & BPF_IOGRAPH_CEDGE_LITERAL) {
				if (len - i < edge->lit_len ||
				    memcmp(buf + i,
					   graph->compact_lits + edge->lit_off,
					   edge->lit_len))
					break;
				i += edge->lit_len;
			}
			if (edge->flags & BPF_IOGRAPH_CEDGE_FINAL_ACTION)
				return edge->dst;
			node = &graph->compact_nodes[edge->dst];
		}

		if (node->action_code) {
			action = node->action_code;
			if (node->flags & IOG_NODE_F_FINAL_ACTION)
				return action;
		}
	}

	return action;
}

__bpf_kfunc int bpf_iograph_run(struct bpf_map *map, const u8 *buf,
				u32 buf__sz, u32 entry_id,
				struct bpf_iograph_run_result *result__uninit)
{
	struct bpf_iograph_map *imap;
	struct bpf_iograph_graph *graph;
	u32 len = buf__sz;
	u32 state = 0;
	int ret = -ENOENT;

	if (!map || map->map_type != BPF_MAP_TYPE_IOGRAPH ||
	    (!buf && len) || !result__uninit)
		return -EINVAL;
	result__uninit->final_state = 0;
	result__uninit->action_code = 0;

	imap = container_of(map, struct bpf_iograph_map, map);
	rcu_read_lock();
	graph = iograph_active_graph(imap);
	if (!graph)
		goto out;
	if (len > graph->max_input_len) {
		ret = -E2BIG;
		goto out;
	}
	ret = iograph_entry_state(graph, entry_id, &state);
	if (ret)
		goto out;

	result__uninit->action_code =
		iograph_walk_result(graph, buf, len, state,
				    &result__uninit->final_state);
	ret = 0;
out:
	rcu_read_unlock();
	return ret;
}

__bpf_kfunc u32 bpf_iograph_run_action(struct bpf_map *map, const u8 *buf,
				       u32 buf__sz, u32 entry_id)
{
	struct bpf_iograph_map *imap;
	struct bpf_iograph_graph *graph;
	u32 len = buf__sz;
	u32 state, action = 0;

	if (!map || map->map_type != BPF_MAP_TYPE_IOGRAPH || (!buf && len))
		return 0;

	imap = container_of(map, struct bpf_iograph_map, map);
	rcu_read_lock();
	graph = iograph_active_graph(imap);
	if (!graph || len > graph->max_input_len ||
	    iograph_compact_entry_state(graph, entry_id, &state))
		goto out;

	action = iograph_walk_action_compact(graph, buf, len, state);
out:
	rcu_read_unlock();
	return action;
}

__bpf_kfunc u32 bpf_iograph_step(struct bpf_map *map, u32 state, u32 sym)
{
	struct bpf_iograph_map *imap;
	struct bpf_iograph_graph *graph;
	u32 next = IOG_NO_STATE;

	if (!map || map->map_type != BPF_MAP_TYPE_IOGRAPH || sym > U8_MAX)
		return IOG_NO_STATE;

	imap = container_of(map, struct bpf_iograph_map, map);
	rcu_read_lock();
	graph = iograph_active_graph(imap);
	if (!graph || state >= graph->hdr->node_cnt)
		goto out;

	next = iograph_step_state(graph, state, (u8)sym);
out:
	rcu_read_unlock();
	return next;
}

BTF_KFUNCS_START(iograph_kfunc_ids)
BTF_ID_FLAGS(func, bpf_iograph_step)
BTF_ID_FLAGS(func, bpf_iograph_run)
BTF_ID_FLAGS(func, bpf_iograph_run_action)
BTF_KFUNCS_END(iograph_kfunc_ids)

static const struct btf_kfunc_id_set iograph_kfunc_set = {
	.owner = THIS_MODULE,
	.set = &iograph_kfunc_ids,
};

static int __init iograph_kfunc_init(void)
{
	return register_btf_kfunc_id_set(BPF_PROG_TYPE_UNSPEC,
					 &iograph_kfunc_set);
}
late_initcall(iograph_kfunc_init);
