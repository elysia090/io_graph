#ifndef IOG_GRAPH_H
#define IOG_GRAPH_H

#include <iog/blob.h>
#include <stdbool.h>

struct iog_graph {
	const void *blob;
	size_t blob_len;
	u32 max_input_len;
	u32 single_entry_id;
	u32 single_entry_state;
	bool single_entry;
	bool accept_codes_inline;
	const struct iog_blob_hdr *hdr;
	const struct iog_node *nodes;
	const struct iog_edge *edges;
	const struct iog_entry *entries;
	const struct iog_accept *accepts;
};

struct iog_graph_obj {
	void *blob;
	size_t blob_len;
	struct iog_graph graph;
	struct iog_graph_obj *next_retired;
};

struct iog_layout_stats {
	u32 zero_edge_nodes;
	u32 single_edge_nodes;
	u32 small_fanout_nodes;
	u32 binary_fanout_nodes;
	u32 max_fanout;
};

int iog_graph_from_blob(struct iog_graph *graph, const void *blob,
			size_t len, const struct iog_limits *limits,
			char *err, size_t err_len);
int iog_graph_obj_new(const void *blob, size_t len,
		      const struct iog_limits *limits,
		      struct iog_graph_obj **obj_out,
		      char *err, size_t err_len);
int iog_graph_inline_accept_codes(struct iog_graph *graph);
void iog_graph_obj_free(struct iog_graph_obj *obj);
int iog_graph_layout_stats(const struct iog_graph *graph,
			   struct iog_layout_stats *stats);

#endif
