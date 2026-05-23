#include "iog_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int iog_graph_from_blob(struct iog_graph *graph, const void *blob,
			size_t len, const struct iog_limits *limits,
			char *err, size_t err_len)
{
	const struct iog_blob_hdr *hdr = blob;
	int ret;

	if (!graph)
		return -EINVAL;

	ret = iog_verify_blob(blob, len, limits, err, err_len);
	if (ret)
		return ret;

	memset(graph, 0, sizeof(*graph));
	graph->blob = blob;
	graph->blob_len = len;
	graph->hdr = hdr;
	graph->nodes = (const void *)((const u8 *)blob + hdr->nodes_off);
	graph->edges = (const void *)((const u8 *)blob + hdr->edges_off);
	graph->entries = (const void *)((const u8 *)blob + hdr->entries_off);
	graph->accepts = (const void *)((const u8 *)blob + hdr->accepts_off);
	graph->max_input_len = hdr->max_input_len;
	if (hdr->entry_cnt == 1) {
		graph->single_entry_id = graph->entries[0].id;
		graph->single_entry_state = graph->entries[0].state;
		graph->single_entry = true;
	}
	return 0;
}

int iog_graph_obj_new(const void *blob, size_t len,
		      const struct iog_limits *limits,
		      struct iog_graph_obj **obj_out,
		      char *err, size_t err_len)
{
	struct iog_graph_obj *obj;
	int ret;

	if (!blob || !len || !obj_out)
		return -EINVAL;

	obj = calloc(1, sizeof(*obj));
	if (!obj)
		return -ENOMEM;

	obj->blob = malloc(len);
	if (!obj->blob) {
		free(obj);
		return -ENOMEM;
	}

	memcpy(obj->blob, blob, len);
	obj->blob_len = len;
	ret = iog_graph_from_blob(&obj->graph, obj->blob, obj->blob_len,
				  limits, err, err_len);
	if (ret) {
		iog_graph_obj_free(obj);
		return ret;
	}
	ret = iog_graph_inline_accept_codes(&obj->graph);
	if (ret) {
		iog_graph_obj_free(obj);
		return ret;
	}

	*obj_out = obj;
	return 0;
}

int iog_graph_inline_accept_codes(struct iog_graph *graph)
{
	struct iog_node *nodes;
	u32 i;

	if (!graph || !graph->hdr || !graph->nodes || !graph->accepts)
		return -EINVAL;
	if (graph->accept_codes_inline)
		return 0;

	nodes = (struct iog_node *)(void *)graph->nodes;
	for (i = 0; i < graph->hdr->node_cnt; i++) {
		u32 accept_id = nodes[i].accept_id;

		nodes[i].accept_id = accept_id ? graph->accepts[accept_id].code :
					     0;
	}
	graph->accept_codes_inline = true;
	return 0;
}

void iog_graph_obj_free(struct iog_graph_obj *obj)
{
	if (!obj)
		return;

	free(obj->blob);
	free(obj);
}

int iog_graph_layout_stats(const struct iog_graph *graph,
			   struct iog_layout_stats *stats)
{
	u32 i;

	if (!graph || !graph->hdr || !stats)
		return -EINVAL;

	memset(stats, 0, sizeof(*stats));
	for (i = 0; i < graph->hdr->node_cnt; i++) {
		u32 fanout = graph->nodes[i].edge_cnt;

		if (!fanout)
			stats->zero_edge_nodes++;
		else if (fanout == 1)
			stats->single_edge_nodes++;
		else if (fanout <= 4)
			stats->small_fanout_nodes++;
		else
			stats->binary_fanout_nodes++;

		if (fanout > stats->max_fanout)
			stats->max_fanout = fanout;
	}

	return 0;
}
