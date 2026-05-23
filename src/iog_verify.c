#include "iog_internal.h"

#include <errno.h>

int iog_verify_blob(const void *blob, size_t len,
		    const struct iog_limits *limits,
		    char *err, size_t err_len)
{
	const struct iog_blob_hdr *hdr = blob;
	const struct iog_node *nodes;
	const struct iog_edge *edges;
	const struct iog_entry *entries;
	const struct iog_accept *accepts;
	size_t accepts_end;
	u32 i;

	if (!blob || len < sizeof(*hdr)) {
		iog_set_err(err, err_len, "blob is smaller than header");
		return -EINVAL;
	}

	if (!limits)
		limits = &iog_default_limits;

	if (hdr->magic != IOG_MAGIC) {
		iog_set_err(err, err_len, "bad magic");
		return -EINVAL;
	}
	if (hdr->version != IOG_VERSION) {
		iog_set_err(err, err_len, "unsupported version");
		return -EINVAL;
	}
	if (hdr->flags) {
		iog_set_err(err, err_len, "unknown flags");
		return -EINVAL;
	}
	if (hdr->reserved) {
		iog_set_err(err, err_len, "reserved field must be zero");
		return -EINVAL;
	}
	if (hdr->total_size != len) {
		iog_set_err(err, err_len, "total_size mismatch");
		return -EINVAL;
	}
	if (!hdr->node_cnt || hdr->initial_state >= hdr->node_cnt) {
		iog_set_err(err, err_len, "initial state out of range");
		return -EINVAL;
	}

	if (hdr->node_cnt > limits->max_nodes ||
	    hdr->edge_cnt > limits->max_edges ||
	    hdr->entry_cnt > limits->max_entries ||
	    hdr->accept_cnt > limits->max_accepts ||
	    !hdr->alphabet_size ||
	    hdr->alphabet_size > limits->max_alphabet ||
	    hdr->max_input_len > limits->max_input_len) {
		iog_set_err(err, err_len, "blob exceeds verifier limits");
		return -E2BIG;
	}

	if ((hdr->nodes_off | hdr->edges_off |
	     hdr->entries_off | hdr->accepts_off) & 3) {
		iog_set_err(err, err_len, "section offset is not 4-byte aligned");
		return -EINVAL;
	}

	if (!iog_section_fits(len, hdr->nodes_off, hdr->node_cnt,
			      sizeof(struct iog_node)) ||
	    !iog_section_fits(len, hdr->edges_off, hdr->edge_cnt,
			      sizeof(struct iog_edge)) ||
	    !iog_section_fits(len, hdr->entries_off, hdr->entry_cnt,
			      sizeof(struct iog_entry)) ||
	    !iog_section_fits(len, hdr->accepts_off, hdr->accept_cnt,
			      sizeof(struct iog_accept))) {
		iog_set_err(err, err_len, "section out of bounds");
		return -EINVAL;
	}

	if (hdr->nodes_off != sizeof(*hdr) ||
	    hdr->edges_off != hdr->nodes_off +
			      hdr->node_cnt * sizeof(struct iog_node) ||
	    hdr->entries_off != hdr->edges_off +
				hdr->edge_cnt * sizeof(struct iog_edge) ||
	    hdr->accepts_off != hdr->entries_off +
				hdr->entry_cnt * sizeof(struct iog_entry)) {
		iog_set_err(err, err_len, "non-canonical section order");
		return -EINVAL;
	}
	accepts_end = hdr->accepts_off +
		      (size_t)hdr->accept_cnt * sizeof(struct iog_accept);
	if (accepts_end != hdr->total_size) {
		iog_set_err(err, err_len, "trailing bytes after accepts");
		return -EINVAL;
	}

	nodes = (const void *)((const u8 *)blob + hdr->nodes_off);
	edges = (const void *)((const u8 *)blob + hdr->edges_off);
	entries = (const void *)((const u8 *)blob + hdr->entries_off);
	accepts = (const void *)((const u8 *)blob + hdr->accepts_off);

	for (i = 0; i < hdr->accept_cnt; i++) {
		if (accepts[i].id != i) {
			iog_set_err(err, err_len, "accept ids are not dense");
			return -EINVAL;
		}
	}

	for (i = 0; i < hdr->entry_cnt; i++) {
		if (entries[i].state >= hdr->node_cnt) {
			iog_set_err(err, err_len, "entry state out of range");
			return -EINVAL;
		}
	}

	for (i = 0; i < hdr->node_cnt; i++) {
		const struct iog_node *node = &nodes[i];
		u32 prev_hi = 0;
		bool have_prev = false;
		u32 j;

		if (node->flags & ~IOG_NODE_FLAG_MASK) {
			iog_set_err(err, err_len, "unknown node flags");
			return -EINVAL;
		}
		if ((node->flags & IOG_NODE_F_FINAL_ACTION) &&
		    !node->accept_id) {
			iog_set_err(err, err_len,
				    "final-action node has no accept id");
			return -EINVAL;
		}
		if (node->edge_start > hdr->edge_cnt ||
		    node->edge_cnt > hdr->edge_cnt - node->edge_start) {
			iog_set_err(err, err_len, "node edge range out of bounds");
			return -EINVAL;
		}
		if (node->default_dst != IOG_NO_STATE &&
		    node->default_dst >= hdr->node_cnt) {
			iog_set_err(err, err_len, "default dst out of range");
			return -EINVAL;
		}
		if (node->accept_id && node->accept_id >= hdr->accept_cnt) {
			iog_set_err(err, err_len, "accept id out of range");
			return -EINVAL;
		}

		for (j = 0; j < node->edge_cnt; j++) {
			const struct iog_edge *edge =
				&edges[node->edge_start + j];

			if (edge->sym_lo > edge->sym_hi ||
			    edge->sym_hi >= hdr->alphabet_size) {
				iog_set_err(err, err_len, "bad edge symbol range");
				return -EINVAL;
			}
			if (edge->dst >= hdr->node_cnt) {
				iog_set_err(err, err_len, "edge dst out of range");
				return -EINVAL;
			}
			if (have_prev && edge->sym_lo <= prev_hi) {
				iog_set_err(err, err_len,
					    "edges overlap or are unsorted");
				return -EINVAL;
			}
			prev_hi = edge->sym_hi;
			have_prev = true;
		}
	}

	return 0;
}
