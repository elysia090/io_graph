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

#define IOG_CREATE_FLAG_MASK	BPF_F_NUMA_NODE

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

static int iog_verify_blob(const void *blob, u32 len)
{
	const struct iog_blob_hdr *hdr = blob;
	const u8 *base = blob;
	const struct iog_node *nodes;
	const struct iog_edge *edges;
	const struct iog_entry *entries;
	const struct iog_accept *accepts;
	u32 expected_off;
	u32 i;

	if (!blob || len < sizeof(*hdr))
		return -EINVAL;
	if (hdr->magic != IOG_MAGIC || hdr->version != IOG_VERSION)
		return -EINVAL;
	if (hdr->flags || hdr->reserved)
		return -EINVAL;
	if (hdr->total_size != len)
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
	if (!iog_u32_array_fits(len, hdr->nodes_off, hdr->node_cnt,
				sizeof(struct iog_node)) ||
	    !iog_u32_array_fits(len, hdr->edges_off, hdr->edge_cnt,
				sizeof(struct iog_edge)) ||
	    !iog_u32_array_fits(len, hdr->entries_off, hdr->entry_cnt,
				sizeof(struct iog_entry)) ||
	    !iog_u32_array_fits(len, hdr->accepts_off, hdr->accept_cnt,
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

		if (node->flags)
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

	for (i = 0; i < hdr->node_cnt; i++) {
		u32 state = i, depth;

		for (depth = 0; depth <= IOG_MAX_DEFAULT_DEPTH; depth++) {
			u32 next = nodes[state].default_dst;

			if (next == IOG_NO_STATE)
				break;
			if (depth == IOG_MAX_DEFAULT_DEPTH)
				return -EINVAL;
			state = next;
		}
	}

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
	graph->accept_codes_inline = true;
}

static struct bpf_iograph_graph *iograph_graph_alloc(const void *value,
						     u32 value_size,
						     int numa_node)
{
	struct bpf_iograph_graph *graph;
	int ret;

	ret = iog_verify_blob(value, value_size);
	if (ret)
		return ERR_PTR(ret);

	graph = kvzalloc_node(struct_size(graph, blob, value_size),
			      GFP_KERNEL_ACCOUNT, numa_node);
	if (!graph)
		return ERR_PTR(-ENOMEM);

	memcpy(graph->blob, value, value_size);
	graph->blob_len = value_size;
	graph->hdr = (const void *)graph->blob;
	graph->nodes = (const void *)(graph->blob + graph->hdr->nodes_off);
	graph->edges = (const void *)(graph->blob + graph->hdr->edges_off);
	graph->entries = (const void *)(graph->blob + graph->hdr->entries_off);
	graph->accepts = (const void *)(graph->blob + graph->hdr->accepts_off);
	graph->max_input_len = graph->hdr->max_input_len;
	iograph_graph_inline_accept_codes(graph);
	if (graph->hdr->entry_cnt == 1) {
		graph->single_entry_id = graph->entries[0].id;
		graph->single_entry_state = graph->entries[0].state;
		graph->single_entry = true;
	}
	return graph;
}

static void iograph_graph_free_rcu(struct rcu_head *rcu)
{
	struct bpf_iograph_graph *graph;

	graph = container_of(rcu, struct bpf_iograph_graph, rcu);
	kvfree(graph);
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
		kvfree(graph);
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
	new_graph = iograph_graph_alloc(value, map->value_size, map->numa_node);
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
	kvfree(new_graph);
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
	if (graph)
		usage += struct_size(graph, blob, graph->blob_len);
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
