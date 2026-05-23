#ifndef IOG_MAP_H
#define IOG_MAP_H

#include <iog/interp.h>

#define IOG_BPF_ANY		0u
#define IOG_BPF_NOEXIST	1u
#define IOG_BPF_EXIST		2u
#define IOG_BPF_KEY_SIZE	((u32)sizeof(u32))
#define IOG_BPF_MAX_ENTRIES	1u

struct iog_map {
	struct iog_graph_obj *graph;
	struct iog_graph_obj *retired;
	u32 retired_cnt;
	u64 update_seq;
};

struct iog_bpf_attr {
	u32 key_size;
	u32 value_size;
	u32 max_entries;
	u32 map_flags;
};

struct iog_bpf_map {
	struct iog_map map;
	struct iog_bpf_attr attr;
};

struct iog_bpf_map_ops {
	int (*map_alloc)(const struct iog_bpf_attr *attr,
			 struct iog_bpf_map **map_out);
	void (*map_free)(struct iog_bpf_map *map);
	void *(*map_lookup_elem)(struct iog_bpf_map *map, const void *key);
	long (*map_update_elem)(struct iog_bpf_map *map, const void *key,
				const void *value, u64 flags,
				char *err, size_t err_len);
	long (*map_delete_elem)(struct iog_bpf_map *map, const void *key);
	int (*map_get_next_key)(struct iog_bpf_map *map, const void *key,
				void *next_key);
	u64 (*map_mem_usage)(const struct iog_bpf_map *map);
};

extern const struct iog_bpf_map_ops iog_bpf_map_ops;

void iog_map_init(struct iog_map *map);
void iog_map_destroy(struct iog_map *map);
int iog_map_update_blob(struct iog_map *map, const void *blob, size_t len,
			const struct iog_limits *limits,
			char *err, size_t err_len);
u32 iog_map_reclaim(struct iog_map *map);
u32 iog_map_run_action(const struct iog_map *map, const u8 *buf, u32 len);
int iog_map_layout_stats(const struct iog_map *map,
			 struct iog_layout_stats *stats);
const struct iog_graph *iog_map_active_graph(const struct iog_map *map);
u64 iog_map_active_mem_usage(const struct iog_map *map);
u64 iog_map_retired_mem_usage(const struct iog_map *map);
u64 iog_map_mem_usage(const struct iog_map *map);

int iog_bpf_map_alloc(const struct iog_bpf_attr *attr,
		      struct iog_bpf_map **map_out);
void iog_bpf_map_free(struct iog_bpf_map *map);
void *iog_bpf_map_lookup_elem(struct iog_bpf_map *map, const void *key);
long iog_bpf_map_update_elem(struct iog_bpf_map *map, const void *key,
			     const void *value, u64 flags,
			     char *err, size_t err_len);
long iog_bpf_map_delete_elem(struct iog_bpf_map *map, const void *key);
int iog_bpf_map_get_next_key(struct iog_bpf_map *map, const void *key,
			     void *next_key);
u64 iog_bpf_map_mem_usage(const struct iog_bpf_map *map);
int iog_bpf_kfunc_step(const struct iog_bpf_map *map, u32 state, u32 sym,
		       u32 *next_state, u32 *action_code);
u32 iog_bpf_kfunc_run_action(const struct iog_bpf_map *map, const u8 *buf,
			     u32 len, u32 entry_id);
int iog_bpf_kfunc_run(const struct iog_bpf_map *map, const u8 *buf, u32 len,
		      u32 entry_id, struct iog_run_result *result);

#endif
