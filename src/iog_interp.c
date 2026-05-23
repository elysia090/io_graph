#include "iog_internal.h"

#include <errno.h>
#include <string.h>

static inline u32 iog_node_accept_code(const struct iog_graph *graph,
				       const struct iog_node *node)
{
	u32 id = node->accept_id;

	if (graph->accept_codes_inline)
		return id;
	return id ? graph->accepts[id].code : 0;
}

static inline u32 iog_node_accept_value(const struct iog_graph *graph,
					const struct iog_accept *accepts,
					u32 accept_id)
{
	if (graph->accept_codes_inline)
		return accept_id;
	return accepts[accept_id].code;
}

static inline u32 iog_node_next_state(const struct iog_edge *all_edges,
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

static inline u32 iog_accept_code(const struct iog_graph *graph, u32 state)
{
	return iog_node_accept_code(graph, &graph->nodes[state]);
}

static inline u32 iog_step(const struct iog_graph *graph, u32 state, u8 sym)
{
	return iog_node_next_state(graph->edges, &graph->nodes[state], sym);
}

int iog_graph_step(const struct iog_graph *graph, u32 state, u32 sym,
		   u32 *next_state, u32 *action_code)
{
	u32 next;

	if (!graph || !graph->hdr || !next_state || !action_code ||
	    state >= graph->hdr->node_cnt || sym >= graph->hdr->alphabet_size)
		return -EINVAL;

	next = iog_step(graph, state, (u8)sym);
	*next_state = next;
	*action_code = next == IOG_NO_STATE ? 0 : iog_accept_code(graph, next);
	return 0;
}

static int iog_entry_state(const struct iog_graph *graph, u32 entry_id,
			   u32 *state)
{
	u32 i;

	if (!graph || !state)
		return -EINVAL;

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

int iog_run(const struct iog_graph *graph, const u8 *buf, u32 len,
	    u32 entry_id, struct iog_run_result *result)
{
	u32 state, action, i;
	int ret;

	if (!graph || !graph->hdr || (!buf && len) || !result)
		return -EINVAL;
	if (len > graph->hdr->max_input_len)
		return -E2BIG;

	ret = iog_entry_state(graph, entry_id, &state);
	if (ret)
		return ret;

	action = iog_accept_code(graph, state);
	memset(result, 0, sizeof(*result));
	result->final_state = state;
	result->action_code = action;

	for (i = 0; i < len; i++) {
		u32 next = iog_step(graph, state, buf[i]);
		u32 state_action;

		if (next == IOG_NO_STATE)
			break;

		state = next;
		result->final_state = state;
		result->consumed = i + 1u;
		state_action = iog_accept_code(graph, state);
		if (state_action) {
			action = state_action;
			result->action_code = action;
		}
	}

	return 0;
}

u32 iog_run_action_entry(const struct iog_graph *graph, const u8 *buf,
			 u32 len, u32 entry_id)
{
	const struct iog_accept *accepts;
	const struct iog_edge *edges;
	const struct iog_node *nodes;
	const struct iog_node *node;
	u32 state, action, i;

	if (!graph || !graph->hdr || (!buf && len))
		return 0;
	if (len > graph->max_input_len ||
	    iog_entry_state(graph, entry_id, &state))
		return 0;

	accepts = graph->accepts;
	edges = graph->edges;
	nodes = graph->nodes;
	node = &nodes[state];
	action = iog_node_accept_code(graph, node);

	for (i = 0; i < len; i++) {
		u32 next = iog_node_next_state(edges, node, buf[i]);
		u32 accept_id, state_action;

		if (next == IOG_NO_STATE)
			break;
		node = &nodes[next];
		accept_id = node->accept_id;
		if (likely(!accept_id))
			continue;
		state_action = iog_node_accept_value(graph, accepts, accept_id);
		if (state_action) {
			action = state_action;
			if (node->flags & IOG_NODE_F_FINAL_ACTION)
				return action;
		}
	}

	return action;
}

u32 iog_run_first_action_entry(const struct iog_graph *graph, const u8 *buf,
			       u32 len, u32 entry_id)
{
	const struct iog_accept *accepts;
	const struct iog_edge *edges;
	const struct iog_node *nodes;
	const struct iog_node *node;
	u32 state, action, i;

	if (!graph || !graph->hdr || (!buf && len))
		return 0;
	if (len > graph->max_input_len ||
	    iog_entry_state(graph, entry_id, &state))
		return 0;

	accepts = graph->accepts;
	edges = graph->edges;
	nodes = graph->nodes;
	node = &nodes[state];
	action = iog_node_accept_code(graph, node);
	if (action)
		return action;

	for (i = 0; i < len; i++) {
		u32 next = iog_node_next_state(edges, node, buf[i]);
		u32 accept_id, state_action;

		if (next == IOG_NO_STATE)
			break;
		node = &nodes[next];
		accept_id = node->accept_id;
		if (likely(!accept_id))
			continue;
		state_action = iog_node_accept_value(graph, accepts, accept_id);
		if (state_action)
			return state_action;
	}

	return 0;
}

u32 iog_run_action(const struct iog_graph *graph, const u8 *buf, u32 len)
{
	return iog_run_action_entry(graph, buf, len, 0);
}

static bool prefix_eq_loop(const u8 *buf, u32 len, const u8 *prefix, u32 plen)
{
	u32 i;

	if (len < plen)
		return false;

	for (i = 0; i < plen; i++) {
		if (buf[i] != prefix[i])
			return false;
	}

	return true;
}

u32 iog_generated_chain_match(const struct iog_prefix *prefixes, size_t nr,
			      const u8 *buf, u32 len)
{
	size_t i;

	for (i = 0; i < nr; i++) {
		if (prefix_eq_loop(buf, len, prefixes[i].bytes, prefixes[i].len))
			return prefixes[i].action_code;
	}

	return 0;
}

u32 iog_list_match(const struct iog_prefix *prefixes, size_t nr,
		   const u8 *buf, u32 len)
{
	size_t i;

	for (i = 0; i < nr; i++) {
		if (len >= prefixes[i].len &&
		    !memcmp(buf, prefixes[i].bytes, prefixes[i].len))
			return prefixes[i].action_code;
	}

	return 0;
}
