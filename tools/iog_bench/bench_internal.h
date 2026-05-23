#ifndef IOG_BENCH_INTERNAL_H
#define IOG_BENCH_INTERNAL_H

#include <iog/bench.h>

#define IOG_BENCH_SAMPLE_NR 64
#define IOG_BENCH_RINGBUF_HDR_SZ_MODEL 8u
#define IOG_BENCH_RINGBUF_CAP_MODEL (1u << 20)

struct event_record {
	u64 ts_ns;
	u64 pid_tgid;
	u32 event_type;
	u32 payload_len;
	u32 selector_off;
	u32 selector_len;
	u8 payload[];
};

struct decoded_event {
	const u8 *path;
	const u8 *cmdline;
	const u8 *arg;
	u32 path_len;
	u32 cmdline_len;
	u32 arg_len;
	u32 event_type;
	u64 pid_tgid;
	u64 scratch[8];
};

struct sample {
	u8 bytes[256];
	u32 len;
};

enum workload_kind {
	WORKLOAD_TYPICAL,
	WORKLOAD_SHARED_PREFIX,
	WORKLOAD_LONG_PATH,
};

struct workload {
	struct iog_prefix *prefixes;
	u8 **owned_prefixes;
	size_t nr;
	u64 prefix_bytes;
	struct sample early[IOG_BENCH_SAMPLE_NR];
	struct sample late[IOG_BENCH_SAMPLE_NR];
	struct sample exact[IOG_BENCH_SAMPLE_NR];
	struct sample hit[IOG_BENCH_SAMPLE_NR];
};

struct run_ctx {
	const struct iog_map *map;
	const struct iog_bpf_map *bpf_map;
	const struct iog_prefix *prefixes;
	size_t prefix_nr;
};

size_t event_record_bytes(u32 payload);
size_t ringbuf_record_bytes(u32 payload);
size_t postdrop_intermediate_bytes(u32 payload);

const char *workload_kind_name(enum workload_kind kind);
void workload_init(struct workload *wl, size_t nr, enum workload_kind kind);
void workload_free(struct workload *wl);

u32 match_iog(const struct run_ctx *ctx, const u8 *buf, u32 len);
u32 match_iog_first_action(const struct run_ctx *ctx, const u8 *buf, u32 len);
u32 match_chain(const struct run_ctx *ctx, const u8 *buf, u32 len);
u32 match_list(const struct run_ctx *ctx, const u8 *buf, u32 len);

#endif
