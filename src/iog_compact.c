#include "iog_internal.h"

#include <iog/compact.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

u64 iog_cgraph_mem_bytes(const struct iog_cgraph *cg)
{
	if (!cg)
		return 0;

	return sizeof(*cg) +
	       (u64)cg->entry_cnt * sizeof(*cg->entries) +
	       (u64)cg->node_cnt * sizeof(*cg->nodes) +
	       (u64)cg->edge_cnt * sizeof(*cg->edges) +
	       cg->lit_len;
}

static int cgraph_entry_state(const struct iog_cgraph *cg, u32 entry_id,
			      u32 *state)
{
	u32 i;

	if (likely(cg->single_entry && cg->single_entry_id == entry_id)) {
		*state = cg->single_entry_state;
		return 0;
	}

	for (i = 0; i < cg->entry_cnt; i++) {
		if (cg->entries[i].id == entry_id) {
			*state = cg->entries[i].state;
			return 0;
		}
	}

	return -ENOENT;
}

static const struct iog_cedge *cnode_find_edge(const struct iog_cedge *all_edges,
					       const struct iog_cnode *node,
					       u8 sym)
{
	const struct iog_cedge *edges = &all_edges[node->edge_start];
	u32 lo = 0, hi = node->edge_cnt;
	u32 i;

	if (likely(node->edge_cnt == 1)) {
		const struct iog_cedge *edge = edges;

		if (sym < edge->sym_lo || sym > edge->sym_hi)
			return NULL;
		return edge;
	}

	if (likely(node->edge_cnt <= 4)) {
		for (i = 0; i < node->edge_cnt; i++) {
			const struct iog_cedge *edge = &edges[i];

			if (sym < edge->sym_lo)
				break;
			if (sym <= edge->sym_hi)
				return edge;
		}
		return NULL;
	}

	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;
		const struct iog_cedge *edge = &edges[mid];

		if (sym < edge->sym_lo)
			hi = mid;
		else if (sym > edge->sym_hi)
			lo = mid + 1;
		else
			return edge;
	}

	return NULL;
}

static bool byte_edge(const struct iog_edge *edge)
{
	return edge->sym_lo == edge->sym_hi && edge->sym_hi <= UINT8_MAX;
}

static void keep_entry_states(const struct iog_graph *graph, bool *keep)
{
	u32 i;

	keep[graph->hdr->initial_state] = true;
	for (i = 0; i < graph->hdr->entry_cnt; i++)
		keep[graph->entries[i].state] = true;
}

static bool final_action_leaf(const struct iog_graph *graph, u32 state)
{
	const struct iog_node *node = &graph->nodes[state];

	return node->accept_id &&
	       (node->flags & IOG_NODE_F_FINAL_ACTION) &&
	       node->edge_cnt == 0 && node->default_dst == IOG_NO_STATE;
}

static int build_keep_set(const struct iog_graph *graph, bool *keep,
			  u32 *incoming)
{
	u32 i;

	keep_entry_states(graph, keep);
	for (i = 0; i < graph->hdr->node_cnt; i++) {
		const struct iog_node *node = &graph->nodes[i];
		u32 j;

		if (final_action_leaf(graph, i)) {
			/* Incoming edges can carry this terminal action. */
		} else if (node->accept_id || node->flags ||
			   node->default_dst != IOG_NO_STATE ||
			   node->edge_cnt != 1) {
			keep[i] = true;
		} else if (!byte_edge(&graph->edges[node->edge_start])) {
			keep[i] = true;
		}
		if (node->default_dst != IOG_NO_STATE) {
			incoming[node->default_dst]++;
			keep[node->default_dst] = true;
		}

		for (j = 0; j < node->edge_cnt; j++) {
			const struct iog_edge *edge =
				&graph->edges[node->edge_start + j];

			incoming[edge->dst]++;
			if (!byte_edge(edge) &&
			    !final_action_leaf(graph, edge->dst))
				keep[edge->dst] = true;
		}
	}

	for (i = 0; i < graph->hdr->node_cnt; i++) {
		if (!final_action_leaf(graph, i) && incoming[i] != 1)
			keep[i] = true;
	}

	return 0;
}

static u32 assign_compact_nodes(const struct iog_graph *graph, const bool *keep,
				u32 *orig_to_compact)
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

static u32 chain_len_to_kept(const struct iog_graph *graph, const bool *keep,
			     u32 dst)
{
	u32 nr = 1;

	while (!keep[dst] && !final_action_leaf(graph, dst)) {
		const struct iog_node *node = &graph->nodes[dst];
		const struct iog_edge *edge = &graph->edges[node->edge_start];

		dst = edge->dst;
		nr++;
	}

	return nr;
}

static u32 chain_endpoint(const struct iog_graph *graph, const bool *keep,
			  u32 dst)
{
	while (!keep[dst] && !final_action_leaf(graph, dst)) {
		const struct iog_node *node = &graph->nodes[dst];
		const struct iog_edge *edge = &graph->edges[node->edge_start];

		dst = edge->dst;
	}

	return dst;
}

static void copy_chain_literal_tail(const struct iog_graph *graph,
				    const bool *keep,
				    const struct iog_edge *first, u8 *dst)
{
	const struct iog_edge *edge = first;
	u32 state = edge->dst;

	while (!keep[state] && !final_action_leaf(graph, state)) {
		const struct iog_node *node = &graph->nodes[state];

		edge = &graph->edges[node->edge_start];
		*dst++ = (u8)edge->sym_lo;
		state = edge->dst;
	}
}

static int count_compact_storage(const struct iog_graph *graph,
				 const bool *keep, u32 *edge_cnt,
				 u32 *lit_len)
{
	u32 i, edges = 0, lits = 0;

	for (i = 0; i < graph->hdr->node_cnt; i++) {
		const struct iog_node *node = &graph->nodes[i];
		u32 j;

		if (!keep[i])
			continue;

		for (j = 0; j < node->edge_cnt; j++) {
			const struct iog_edge *edge =
				&graph->edges[node->edge_start + j];
			u32 clen = 1;

			edges++;
			if (byte_edge(edge))
				clen = chain_len_to_kept(graph, keep,
							 edge->dst);
			if (clen > 1) {
				u32 tail_len = clen - 1;

				if (UINT32_MAX - lits < tail_len)
					return -E2BIG;
				lits += tail_len;
			}
		}
	}

	*edge_cnt = edges;
	*lit_len = lits;
	return 0;
}

static int alloc_cgraph(const struct iog_graph *graph, u32 node_cnt,
			u32 edge_cnt, u32 lit_len, struct iog_cgraph **out)
{
	struct iog_cgraph *cg;

	cg = calloc(1, sizeof(*cg));
	if (!cg)
		return -ENOMEM;

	if (graph->hdr->entry_cnt)
		cg->entries = calloc(graph->hdr->entry_cnt,
				     sizeof(*cg->entries));
	cg->nodes = calloc(node_cnt, sizeof(*cg->nodes));
	cg->edges = calloc(edge_cnt ? edge_cnt : 1, sizeof(*cg->edges));
	cg->lits = lit_len ? malloc(lit_len) : NULL;
	if ((graph->hdr->entry_cnt && !cg->entries) || !cg->nodes || !cg->edges ||
	    (lit_len && !cg->lits)) {
		iog_cgraph_free(cg);
		return -ENOMEM;
	}

	cg->entry_cnt = graph->hdr->entry_cnt;
	cg->node_cnt = node_cnt;
	cg->edge_cnt = edge_cnt;
	cg->lit_len = lit_len;
	cg->max_input_len = graph->max_input_len;
	if (graph->hdr->entry_cnt)
		memcpy(cg->entries, graph->entries,
		       (size_t)graph->hdr->entry_cnt * sizeof(*cg->entries));

	*out = cg;
	return 0;
}

static int build_cgraph_edges(const struct iog_graph *graph, const bool *keep,
			      const u32 *orig_to_compact,
			      struct iog_cgraph *cg)
{
	u32 i, edge_pos = 0, lit_pos = 0;

	for (i = 0; i < graph->hdr->entry_cnt; i++)
		cg->entries[i].state = orig_to_compact[cg->entries[i].state];

	for (i = 0; i < graph->hdr->node_cnt; i++) {
		const struct iog_node *node = &graph->nodes[i];
		struct iog_cnode *cnode;
		u32 cstate, j;

		if (!keep[i])
			continue;

		cstate = orig_to_compact[i];
		cnode = &cg->nodes[cstate];
		cnode->edge_start = edge_pos;
		cnode->edge_cnt = node->edge_cnt;
		cnode->flags = node->flags;
		cnode->action_code = node->accept_id;
		cnode->default_dst = node->default_dst == IOG_NO_STATE ?
				     IOG_NO_STATE :
				     orig_to_compact[node->default_dst];

		for (j = 0; j < node->edge_cnt; j++) {
			const struct iog_edge *edge =
				&graph->edges[node->edge_start + j];
			struct iog_cedge *cedge = &cg->edges[edge_pos++];
			u32 clen = 1;
			u32 endpoint = edge->dst;

			cedge->sym_lo = (u8)edge->sym_lo;
			cedge->sym_hi = (u8)edge->sym_hi;
			if (byte_edge(edge)) {
				clen = chain_len_to_kept(graph, keep,
							 edge->dst);
				endpoint = chain_endpoint(graph, keep,
							  edge->dst);
			}
			if (final_action_leaf(graph, endpoint)) {
				cedge->flags |= IOG_CEDGE_FINAL_ACTION;
				cedge->dst = graph->nodes[endpoint].accept_id;
			} else {
				cedge->dst = orig_to_compact[endpoint];
			}
			if (clen > 1) {
				u32 tail_len = clen - 1;

				cedge->flags |= IOG_CEDGE_LITERAL;
				cedge->lit_off = lit_pos;
				cedge->lit_len = tail_len;
				copy_chain_literal_tail(graph, keep, edge,
							&cg->lits[lit_pos]);
				lit_pos += tail_len;
			}
		}
	}

	if (graph->hdr->entry_cnt == 1) {
		cg->single_entry_id = cg->entries[0].id;
		cg->single_entry_state = cg->entries[0].state;
		cg->single_entry = true;
	}

	return 0;
}

int iog_cgraph_new(const struct iog_graph *graph, struct iog_cgraph **out)
{
	struct iog_cgraph *cg = NULL;
	bool *keep = NULL;
	u32 *incoming = NULL;
	u32 *orig_to_compact = NULL;
	u32 node_cnt, edge_cnt = 0, lit_len = 0;
	int ret;

	if (!graph || !graph->hdr || !out)
		return -EINVAL;

	*out = NULL;
	keep = calloc(graph->hdr->node_cnt, sizeof(*keep));
	incoming = calloc(graph->hdr->node_cnt, sizeof(*incoming));
	orig_to_compact = malloc((size_t)graph->hdr->node_cnt *
				 sizeof(*orig_to_compact));
	if (!keep || !incoming || !orig_to_compact) {
		ret = -ENOMEM;
		goto out;
	}

	ret = build_keep_set(graph, keep, incoming);
	if (ret)
		goto out;

	node_cnt = assign_compact_nodes(graph, keep, orig_to_compact);
	ret = count_compact_storage(graph, keep, &edge_cnt, &lit_len);
	if (ret)
		goto out;

	ret = alloc_cgraph(graph, node_cnt, edge_cnt, lit_len, &cg);
	if (ret)
		goto out;

	ret = build_cgraph_edges(graph, keep, orig_to_compact, cg);
	if (ret)
		goto out;

	*out = cg;
	cg = NULL;

out:
	iog_cgraph_free(cg);
	free(orig_to_compact);
	free(incoming);
	free(keep);
	return ret;
}

void iog_cgraph_free(struct iog_cgraph *cg)
{
	if (!cg)
		return;

	free(cg->entries);
	free(cg->nodes);
	free(cg->edges);
	free(cg->lits);
	free(cg);
}

u32 iog_cgraph_run_action_entry(const struct iog_cgraph *cg, const u8 *buf,
				u32 len, u32 entry_id)
{
	const struct iog_cnode *node;
	u32 state, action, i = 0;

	if (!cg || (!buf && len))
		return 0;
	if (len > cg->max_input_len || cgraph_entry_state(cg, entry_id, &state))
		return 0;

	node = &cg->nodes[state];
	action = node->action_code;
	if (action && (node->flags & IOG_NODE_F_FINAL_ACTION))
		return action;

	while (i < len) {
		const struct iog_cedge *edge =
			cnode_find_edge(cg->edges, node, buf[i]);

		if (!edge) {
			if (node->default_dst == IOG_NO_STATE)
				break;
			node = &cg->nodes[node->default_dst];
			i++;
		} else {
			i++;
			if (edge->flags & IOG_CEDGE_LITERAL) {
				if (len - i < edge->lit_len ||
				    memcmp(buf + i, cg->lits + edge->lit_off,
					   edge->lit_len))
					break;
				i += edge->lit_len;
			}
			if (edge->flags & IOG_CEDGE_FINAL_ACTION)
				return edge->dst;
			node = &cg->nodes[edge->dst];
		}

		if (node->action_code) {
			action = node->action_code;
			if (node->flags & IOG_NODE_F_FINAL_ACTION)
				return action;
		}
	}

	return action;
}

u32 iog_cgraph_run_action(const struct iog_cgraph *cg, const u8 *buf, u32 len)
{
	return iog_cgraph_run_action_entry(cg, buf, len, 0);
}

u32 iog_cgraph_count_transitions_entry(const struct iog_cgraph *cg,
				       const u8 *buf, u32 len, u32 entry_id)
{
	const struct iog_cnode *node;
	u32 state, i = 0, transitions = 0;

	if (!cg || (!buf && len))
		return 0;
	if (len > cg->max_input_len ||
	    cgraph_entry_state(cg, entry_id, &state))
		return 0;

	node = &cg->nodes[state];
	while (i < len) {
		const struct iog_cedge *edge =
			cnode_find_edge(cg->edges, node, buf[i]);

		if (!edge) {
			if (node->default_dst == IOG_NO_STATE)
				break;
			node = &cg->nodes[node->default_dst];
			i++;
			transitions++;
			continue;
		}

		i++;
		if (edge->flags & IOG_CEDGE_LITERAL) {
			if (len - i < edge->lit_len ||
			    memcmp(buf + i, cg->lits + edge->lit_off,
				   edge->lit_len))
				break;
			i += edge->lit_len;
		}
		transitions++;
		if (edge->flags & IOG_CEDGE_FINAL_ACTION)
			break;
		node = &cg->nodes[edge->dst];

		if (node->action_code &&
		    (node->flags & IOG_NODE_F_FINAL_ACTION))
			break;
	}

	return transitions;
}

u32 iog_cgraph_count_transitions(const struct iog_cgraph *cg, const u8 *buf,
				 u32 len)
{
	return iog_cgraph_count_transitions_entry(cg, buf, len, 0);
}

int iog_cgraph_stats(const struct iog_cgraph *cg,
		     struct iog_cgraph_stats *stats)
{
	u32 *depth = NULL;
	u32 i;

	if (!cg || !stats)
		return -EINVAL;

	memset(stats, 0, sizeof(*stats));
	stats->nodes = cg->node_cnt;
	stats->edges = cg->edge_cnt;
	stats->literal_bytes = cg->lit_len;
	stats->mem_bytes = iog_cgraph_mem_bytes(cg);

	for (i = 0; i < cg->edge_cnt; i++) {
		const struct iog_cedge *edge = &cg->edges[i];

		if (!(edge->flags & IOG_CEDGE_LITERAL))
			continue;
		stats->literal_edges++;
		if (edge->lit_len > stats->max_literal_len)
			stats->max_literal_len = edge->lit_len;
	}
	for (i = 0; i < cg->node_cnt; i++) {
		if (cg->nodes[i].edge_cnt > stats->max_fanout)
			stats->max_fanout = cg->nodes[i].edge_cnt;
	}

	depth = calloc(cg->node_cnt ? cg->node_cnt : 1, sizeof(*depth));
	if (!depth)
		return -ENOMEM;

	stats->depth_complete = true;
	for (i = cg->node_cnt; i > 0; i--) {
		const struct iog_cnode *node = &cg->nodes[i - 1];
		u32 best = 0;
		u32 j;

		for (j = 0; j < node->edge_cnt; j++) {
			const struct iog_cedge *edge =
				&cg->edges[node->edge_start + j];

			if (edge->flags & IOG_CEDGE_FINAL_ACTION) {
				if (best < 1)
					best = 1;
				continue;
			}
			if (edge->dst <= i - 1 || edge->dst >= cg->node_cnt) {
				stats->depth_complete = false;
				continue;
			}
			if (depth[edge->dst] + 1 > best)
				best = depth[edge->dst] + 1;
		}
		if (node->default_dst != IOG_NO_STATE) {
			if (node->default_dst <= i - 1 ||
			    node->default_dst >= cg->node_cnt) {
				stats->depth_complete = false;
			} else if (depth[node->default_dst] + 1 > best) {
				best = depth[node->default_dst] + 1;
			}
		}
		depth[i - 1] = best;
		if (best > stats->max_depth)
			stats->max_depth = best;
	}

	free(depth);

	return 0;
}
