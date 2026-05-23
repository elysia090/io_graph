#include "iog_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void iog_map_init(struct iog_map *map)
{
	memset(map, 0, sizeof(*map));
}

void iog_map_destroy(struct iog_map *map)
{
	if (!map)
		return;

	iog_graph_obj_free(map->graph);
	iog_map_reclaim(map);
	map->graph = NULL;
	map->retired = NULL;
	map->retired_cnt = 0;
	map->update_seq = 0;
}

static void iog_map_retire_graph(struct iog_map *map,
				 struct iog_graph_obj *old_graph)
{
	if (!map || !old_graph)
		return;

	old_graph->next_retired = map->retired;
	map->retired = old_graph;
	if (map->retired_cnt != UINT32_MAX)
		map->retired_cnt++;
}

int iog_map_update_blob(struct iog_map *map, const void *blob, size_t len,
			const struct iog_limits *limits,
			char *err, size_t err_len)
{
	struct iog_graph_obj *new_graph, *old_graph;
	int ret;

	if (!map)
		return -EINVAL;

	ret = iog_graph_obj_new(blob, len, limits, &new_graph, err, err_len);
	if (ret)
		return ret;

	old_graph = map->graph;
	map->graph = new_graph;
	map->update_seq++;
	iog_map_retire_graph(map, old_graph);
	return 0;
}

u32 iog_map_reclaim(struct iog_map *map)
{
	struct iog_graph_obj *obj, *next;
	u32 nr = 0;

	if (!map)
		return 0;

	obj = map->retired;
	map->retired = NULL;
	map->retired_cnt = 0;

	while (obj) {
		next = obj->next_retired;
		obj->next_retired = NULL;
		iog_graph_obj_free(obj);
		obj = next;
		nr++;
	}

	return nr;
}

u32 iog_map_run_action(const struct iog_map *map, const u8 *buf, u32 len)
{
	const struct iog_graph_obj *obj = map->graph;

	if (unlikely(!obj))
		return 0;

	return iog_run_action(&obj->graph, buf, len);
}

int iog_map_layout_stats(const struct iog_map *map,
			 struct iog_layout_stats *stats)
{
	const struct iog_graph_obj *obj;

	if (!map || !stats)
		return -EINVAL;

	obj = map->graph;
	if (!obj)
		return -ENOENT;

	return iog_graph_layout_stats(&obj->graph, stats);
}

const struct iog_graph *iog_map_active_graph(const struct iog_map *map)
{
	if (!map || !map->graph)
		return NULL;

	return &map->graph->graph;
}

static u64 iog_graph_obj_mem_usage(const struct iog_graph_obj *obj)
{
	return obj ? sizeof(*obj) + obj->blob_len : 0;
}

u64 iog_map_active_mem_usage(const struct iog_map *map)
{
	if (!map)
		return 0;

	return sizeof(*map) + iog_graph_obj_mem_usage(map->graph);
}

u64 iog_map_retired_mem_usage(const struct iog_map *map)
{
	const struct iog_graph_obj *obj;
	u64 bytes = 0;

	if (!map)
		return 0;

	for (obj = map->retired; obj; obj = obj->next_retired)
		bytes += iog_graph_obj_mem_usage(obj);

	return bytes;
}

u64 iog_map_mem_usage(const struct iog_map *map)
{
	if (!map)
		return 0;

	return iog_map_active_mem_usage(map) +
	       iog_map_retired_mem_usage(map);
}

static bool iog_bpf_key_ok(const void *key)
{
	return key && *(const u32 *)key == 0;
}

int iog_bpf_map_alloc(const struct iog_bpf_attr *attr,
		      struct iog_bpf_map **map_out)
{
	struct iog_bpf_map *map;

	if (!attr || !map_out)
		return -EINVAL;
	if (attr->key_size != IOG_BPF_KEY_SIZE ||
	    !attr->value_size ||
	    attr->max_entries != IOG_BPF_MAX_ENTRIES ||
	    attr->map_flags)
		return -EINVAL;

	map = calloc(1, sizeof(*map));
	if (!map)
		return -ENOMEM;

	iog_map_init(&map->map);
	map->attr = *attr;
	*map_out = map;
	return 0;
}

void iog_bpf_map_free(struct iog_bpf_map *map)
{
	if (!map)
		return;

	iog_map_destroy(&map->map);
	free(map);
}

void *iog_bpf_map_lookup_elem(struct iog_bpf_map *map, const void *key)
{
	(void)map;
	(void)key;
	errno = ENOTSUP;
	return NULL;
}

long iog_bpf_map_update_elem(struct iog_bpf_map *map, const void *key,
			     const void *value, u64 flags,
			     char *err, size_t err_len)
{
	if (!map || !iog_bpf_key_ok(key) || !value)
		return -EINVAL;
	if (flags != IOG_BPF_ANY &&
	    flags != IOG_BPF_NOEXIST &&
	    flags != IOG_BPF_EXIST)
		return -EINVAL;
	if (flags == IOG_BPF_NOEXIST && map->map.graph)
		return -EEXIST;
	if (flags == IOG_BPF_EXIST && !map->map.graph)
		return -ENOENT;

	return iog_map_update_blob(&map->map, value, map->attr.value_size,
				   &iog_default_limits, err, err_len);
}

long iog_bpf_map_delete_elem(struct iog_bpf_map *map, const void *key)
{
	struct iog_graph_obj *old_graph;

	if (!map || !iog_bpf_key_ok(key))
		return -EINVAL;
	if (!map->map.graph)
		return -ENOENT;

	old_graph = map->map.graph;
	map->map.graph = NULL;
	map->map.update_seq++;
	iog_map_retire_graph(&map->map, old_graph);
	return 0;
}

int iog_bpf_map_get_next_key(struct iog_bpf_map *map, const void *key,
			     void *next_key)
{
	(void)map;
	(void)key;
	(void)next_key;
	return -ENOTSUP;
}

u64 iog_bpf_map_mem_usage(const struct iog_bpf_map *map)
{
	if (!map)
		return 0;

	return sizeof(*map) + iog_map_active_mem_usage(&map->map) -
	       sizeof(map->map) + iog_map_retired_mem_usage(&map->map);
}

int iog_bpf_kfunc_step(const struct iog_bpf_map *map, u32 state, u32 sym,
		       u32 *next_state, u32 *action_code)
{
	const struct iog_graph *graph;

	if (!map)
		return -EINVAL;

	graph = iog_map_active_graph(&map->map);
	if (!graph)
		return -ENOENT;

	return iog_graph_step(graph, state, sym, next_state, action_code);
}

u32 iog_bpf_kfunc_run_action(const struct iog_bpf_map *map, const u8 *buf,
			     u32 len, u32 entry_id)
{
	const struct iog_graph *graph;

	if (!map)
		return 0;

	graph = iog_map_active_graph(&map->map);
	if (!graph)
		return 0;

	return iog_run_action_entry(graph, buf, len, entry_id);
}

int iog_bpf_kfunc_run(const struct iog_bpf_map *map, const u8 *buf, u32 len,
		      u32 entry_id, struct iog_run_result *result)
{
	const struct iog_graph *graph;

	if (!map)
		return -EINVAL;

	graph = iog_map_active_graph(&map->map);
	if (!graph)
		return -ENOENT;

	return iog_run(graph, buf, len, entry_id, result);
}

const struct iog_bpf_map_ops iog_bpf_map_ops = {
	.map_alloc = iog_bpf_map_alloc,
	.map_free = iog_bpf_map_free,
	.map_lookup_elem = iog_bpf_map_lookup_elem,
	.map_update_elem = iog_bpf_map_update_elem,
	.map_delete_elem = iog_bpf_map_delete_elem,
	.map_get_next_key = iog_bpf_map_get_next_key,
	.map_mem_usage = iog_bpf_map_mem_usage,
};
