#include "iog_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct build_edge {
	u8 sym;
	u32 dst;
};

struct build_node {
	struct build_edge *edges;
	u16 edge_cnt;
	u16 edge_cap;
	u32 accept_id;
};

static void free_build_nodes(struct build_node *nodes, u32 nr)
{
	u32 i;

	if (!nodes)
		return;

	for (i = 0; i < nr; i++)
		free(nodes[i].edges);
	free(nodes);
}

static int grow_nodes(struct build_node **nodes, u32 *cap, u32 need)
{
	struct build_node *new_nodes;
	u32 new_cap = *cap ? *cap : 128;

	while (new_cap < need) {
		if (new_cap > UINT32_MAX / 2)
			return -EOVERFLOW;
		new_cap *= 2;
	}

	new_nodes = realloc(*nodes, (size_t)new_cap * sizeof(*new_nodes));
	if (!new_nodes)
		return -ENOMEM;

	memset(new_nodes + *cap, 0, (size_t)(new_cap - *cap) * sizeof(*new_nodes));
	*nodes = new_nodes;
	*cap = new_cap;
	return 0;
}

static int grow_edges(struct build_node *node)
{
	struct build_edge *edges;
	u16 new_cap = node->edge_cap ? (u16)(node->edge_cap * 2) : 4;

	if (node->edge_cap > UINT16_MAX / 2)
		return -EOVERFLOW;

	edges = realloc(node->edges, (size_t)new_cap * sizeof(*edges));
	if (!edges)
		return -ENOMEM;

	node->edges = edges;
	node->edge_cap = new_cap;
	return 0;
}

static int find_or_add_edge(struct build_node **nodes, u32 *node_cnt,
			    u32 *node_cap, u32 state, u8 sym, u32 *dst)
{
	struct build_node *node = &(*nodes)[state];
	u16 i, pos;
	int ret;

	for (i = 0; i < node->edge_cnt; i++) {
		if (node->edges[i].sym == sym) {
			*dst = node->edges[i].dst;
			return 0;
		}
		if (node->edges[i].sym > sym)
			break;
	}

	if (*node_cnt == UINT32_MAX)
		return -EOVERFLOW;

	if (*node_cnt + 1 > *node_cap) {
		ret = grow_nodes(nodes, node_cap, *node_cnt + 1);
		if (ret)
			return ret;
		node = &(*nodes)[state];
	}

	if (node->edge_cnt == node->edge_cap) {
		ret = grow_edges(node);
		if (ret)
			return ret;
	}

	pos = i;
	if (pos < node->edge_cnt) {
		memmove(&node->edges[pos + 1], &node->edges[pos],
			(size_t)(node->edge_cnt - pos) * sizeof(node->edges[0]));
	}

	*dst = *node_cnt;
	node->edges[pos].sym = sym;
	node->edges[pos].dst = *dst;
	node->edge_cnt++;
	(*node_cnt)++;
	return 0;
}

static int accept_id_for_code(u32 action_code, u32 **codes, u32 *cnt, u32 *cap,
			      u32 *id)
{
	u32 i, new_cap;
	u32 *new_codes;

	for (i = 0; i < *cnt; i++) {
		if ((*codes)[i] == action_code) {
			*id = i;
			return 0;
		}
	}

	if (*cnt == UINT32_MAX)
		return -EOVERFLOW;

	if (*cnt == *cap) {
		new_cap = *cap ? *cap * 2 : 8;
		if (new_cap < *cap)
			return -EOVERFLOW;
		new_codes = realloc(*codes, (size_t)new_cap * sizeof(*new_codes));
		if (!new_codes)
			return -ENOMEM;
		*codes = new_codes;
		*cap = new_cap;
	}

	*id = *cnt;
	(*codes)[*cnt] = action_code;
	(*cnt)++;
	return 0;
}

static int compute_total_size(u32 node_cnt, u32 edge_cnt, u32 entry_cnt,
			      u32 accept_cnt, size_t *total)
{
	size_t size, bytes;

	size = sizeof(struct iog_blob_hdr);
	if (iog_mul_overflow_size(node_cnt, sizeof(struct iog_node), &bytes) ||
	    iog_add_overflow_size(size, bytes, &size))
		return -EOVERFLOW;
	if (iog_mul_overflow_size(edge_cnt, sizeof(struct iog_edge), &bytes) ||
	    iog_add_overflow_size(size, bytes, &size))
		return -EOVERFLOW;
	if (iog_mul_overflow_size(entry_cnt, sizeof(struct iog_entry), &bytes) ||
	    iog_add_overflow_size(size, bytes, &size))
		return -EOVERFLOW;
	if (iog_mul_overflow_size(accept_cnt, sizeof(struct iog_accept), &bytes) ||
	    iog_add_overflow_size(size, bytes, &size))
		return -EOVERFLOW;
	if (size > UINT32_MAX)
		return -EOVERFLOW;

	*total = size;
	return 0;
}

int iog_compile_prefixes(const struct iog_prefix *prefixes, size_t nr,
			 u32 max_input_len, void **blob_out,
			 size_t *blob_len_out,
			 struct iog_compile_stats *stats,
			 char *err, size_t err_len)
{
	struct build_node *nodes = NULL;
	struct iog_blob_hdr *hdr;
	struct iog_node *out_nodes;
	struct iog_edge *out_edges;
	struct iog_entry *entries;
	struct iog_accept *accepts;
	u32 node_cap = 0, node_cnt = 0;
	u32 edge_cnt = 0, accept_cnt = 0, accept_cap = 0;
	u32 *accept_codes = NULL;
	size_t prefix_bytes = 0, total_size;
	void *blob = NULL;
	u32 i, j, edge_pos = 0;
	int ret;

	if (!prefixes || !nr || !blob_out || !blob_len_out)
		return -EINVAL;
	if (nr > UINT32_MAX)
		return -EOVERFLOW;

	ret = grow_nodes(&nodes, &node_cap, 1);
	if (ret)
		goto out;
	node_cnt = 1;

	ret = accept_id_for_code(0, &accept_codes, &accept_cnt, &accept_cap, &i);
	if (ret)
		goto out;

	for (i = 0; i < nr; i++) {
		u32 state = 0, accept_id;

		if (!prefixes[i].bytes || !prefixes[i].len) {
			ret = -EINVAL;
			iog_set_err(err, err_len, "empty prefix at %u", i);
			goto out;
		}

		if (SIZE_MAX - prefix_bytes < prefixes[i].len) {
			ret = -EOVERFLOW;
			goto out;
		}
		prefix_bytes += prefixes[i].len;

		ret = accept_id_for_code(prefixes[i].action_code, &accept_codes,
					 &accept_cnt, &accept_cap, &accept_id);
		if (ret)
			goto out;

		for (j = 0; j < prefixes[i].len; j++) {
			u32 dst;

			ret = find_or_add_edge(&nodes, &node_cnt, &node_cap,
					       state, prefixes[i].bytes[j],
					       &dst);
			if (ret)
				goto out;
			state = dst;
		}

		nodes[state].accept_id = accept_id;
	}

	for (i = 0; i < node_cnt; i++)
		edge_cnt += nodes[i].edge_cnt;

	ret = compute_total_size(node_cnt, edge_cnt, 1, accept_cnt, &total_size);
	if (ret)
		goto out;

	blob = calloc(1, total_size);
	if (!blob) {
		ret = -ENOMEM;
		goto out;
	}

	hdr = blob;
	hdr->magic = IOG_MAGIC;
	hdr->version = IOG_VERSION;
	hdr->node_cnt = node_cnt;
	hdr->edge_cnt = edge_cnt;
	hdr->entry_cnt = 1;
	hdr->accept_cnt = accept_cnt;
	hdr->alphabet_size = 256;
	hdr->initial_state = 0;
	hdr->nodes_off = sizeof(*hdr);
	hdr->edges_off = hdr->nodes_off + node_cnt * sizeof(struct iog_node);
	hdr->entries_off = hdr->edges_off + edge_cnt * sizeof(struct iog_edge);
	hdr->accepts_off = hdr->entries_off + sizeof(struct iog_entry);
	hdr->total_size = (u32)total_size;
	hdr->max_input_len = max_input_len;

	out_nodes = (void *)((u8 *)blob + hdr->nodes_off);
	out_edges = (void *)((u8 *)blob + hdr->edges_off);
	entries = (void *)((u8 *)blob + hdr->entries_off);
	accepts = (void *)((u8 *)blob + hdr->accepts_off);

	for (i = 0; i < node_cnt; i++) {
		out_nodes[i].edge_start = edge_pos;
		out_nodes[i].edge_cnt = nodes[i].edge_cnt;
		if (nodes[i].accept_id && !nodes[i].edge_cnt)
			out_nodes[i].flags = IOG_NODE_F_FINAL_ACTION;
		out_nodes[i].default_dst = IOG_NO_STATE;
		out_nodes[i].accept_id = nodes[i].accept_id;

		for (j = 0; j < nodes[i].edge_cnt; j++) {
			out_edges[edge_pos].sym_lo = nodes[i].edges[j].sym;
			out_edges[edge_pos].sym_hi = nodes[i].edges[j].sym;
			out_edges[edge_pos].dst = nodes[i].edges[j].dst;
			edge_pos++;
		}
	}

	entries[0].id = 0;
	entries[0].state = 0;

	for (i = 0; i < accept_cnt; i++) {
		accepts[i].id = i;
		accepts[i].code = accept_codes[i];
	}

	ret = iog_verify_blob(blob, total_size, &iog_default_limits, err, err_len);
	if (ret)
		goto out;

	if (stats) {
		memset(stats, 0, sizeof(*stats));
		stats->node_cnt = node_cnt;
		stats->edge_cnt = edge_cnt;
		stats->accept_cnt = accept_cnt;
		stats->prefix_bytes = prefix_bytes;
		stats->blob_bytes = total_size;
		stats->dense_table_bytes = iog_dense_table_bytes(node_cnt);
		stats->gen_chain_source_bytes =
			iog_generated_chain_source_bytes(prefixes, nr);
		stats->gen_chain_bpf_bytes =
			iog_generated_chain_bpf_bytes(prefixes, nr);
		stats->list_payload_bytes = iog_list_payload_bytes(prefixes, nr);
	}

	*blob_out = blob;
	*blob_len_out = total_size;
	blob = NULL;
	ret = 0;

out:
	if (ret && err && err_len && !err[0])
		iog_set_err(err, err_len, "compile failed: %s", strerror(-ret));
	free(blob);
	free(accept_codes);
	free_build_nodes(nodes, node_cnt);
	return ret;
}

u64 iog_dense_table_bytes(u32 node_cnt)
{
	return (u64)node_cnt * 256u * sizeof(u32) +
	       (u64)node_cnt * sizeof(u32);
}

static u32 dec_digits_u32(u32 value)
{
	u32 digits = 1;

	while (value >= 10) {
		value /= 10;
		digits++;
	}

	return digits;
}

static u64 c_string_literal_bytes(const u8 *bytes, u32 len)
{
	u64 total = 0;
	u32 i;

	for (i = 0; i < len; i++) {
		u8 c = bytes[i];

		if (c >= 0x20 && c <= 0x7e && c != '\\' && c != '"')
			total++;
		else
			total += 4;
	}

	return total;
}

u64 iog_generated_chain_source_bytes(const struct iog_prefix *prefixes,
				     size_t nr)
{
	u64 bytes = sizeof("static __always_inline u32 match(const u8 *buf, u32 len)\n{\n") - 1;
	size_t i;

	for (i = 0; i < nr; i++) {
		bytes += sizeof("\tif (len >= ") - 1;
		bytes += dec_digits_u32(prefixes[i].len);
		bytes += sizeof(" && !memcmp(buf, \"") - 1;
		bytes += c_string_literal_bytes(prefixes[i].bytes,
						prefixes[i].len);
		bytes += sizeof("\", ") - 1;
		bytes += dec_digits_u32(prefixes[i].len);
		bytes += sizeof("))\n\t\treturn ") - 1;
		bytes += dec_digits_u32(prefixes[i].action_code);
		bytes += sizeof(";\n") - 1;
	}

	bytes += sizeof("\treturn 0;\n}\n") - 1;
	return bytes;
}

u64 iog_generated_chain_bpf_bytes(const struct iog_prefix *prefixes,
				  size_t nr)
{
	u64 insns = 0;
	size_t i;

	for (i = 0; i < nr; i++)
		insns += 2 + (u64)prefixes[i].len * 3 + 2;

	return insns * 8;
}

u64 iog_list_payload_bytes(const struct iog_prefix *prefixes, size_t nr)
{
	u64 bytes = (u64)nr * (sizeof(u32) + sizeof(u32));
	size_t i;

	for (i = 0; i < nr; i++)
		bytes += prefixes[i].len;

	return bytes;
}
