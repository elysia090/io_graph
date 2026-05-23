#include "bench_internal.h"

u32 match_iog(const struct run_ctx *ctx, const u8 *buf, u32 len)
{
	const struct iog_graph *graph = iog_map_active_graph(ctx->map);

	return graph ? iog_run_action(graph, buf, len) : 0;
}

u32 match_iog_compact(const struct run_ctx *ctx, const u8 *buf, u32 len)
{
	return iog_map_run_action(ctx->map, buf, len);
}

u32 match_iog_first_action(const struct run_ctx *ctx, const u8 *buf, u32 len)
{
	const struct iog_graph *graph = iog_map_active_graph(ctx->map);

	return graph ? iog_run_first_action_entry(graph, buf, len, 0) : 0;
}

u32 match_chain(const struct run_ctx *ctx, const u8 *buf, u32 len)
{
	return iog_generated_chain_match(ctx->prefixes, ctx->prefix_nr, buf, len);
}

u32 match_list(const struct run_ctx *ctx, const u8 *buf, u32 len)
{
	return iog_list_match(ctx->prefixes, ctx->prefix_nr, buf, len);
}
