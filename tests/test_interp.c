#include "test_common.h"

struct sample_bytes {
	const u8 *bytes;
	u32 len;
};

static void check_compact_equiv(const struct iog_prefix *prefixes, size_t nr,
				const struct sample_bytes *samples,
				size_t sample_nr)
{
	struct iog_cgraph *compact;
	struct iog_graph graph;
	char err[256] = "";
	void *blob;
	size_t blob_len;
	size_t i;

	TEST_ASSERT(!iog_compile_prefixes(prefixes, nr, 256, &blob, &blob_len,
					  NULL, err, sizeof(err)));
	TEST_ASSERT(!iog_graph_from_blob(&graph, blob, blob_len,
					 &iog_default_limits, err,
					 sizeof(err)));
	TEST_ASSERT(!iog_graph_inline_accept_codes(&graph));
	TEST_ASSERT(!iog_cgraph_new(&graph, &compact));
	for (i = 0; i < sample_nr; i++) {
		u32 expected = iog_run_action(&graph, samples[i].bytes,
					      samples[i].len);
		u32 actual = iog_cgraph_run_action(compact, samples[i].bytes,
						   samples[i].len);

		TEST_ASSERT(expected == actual);
	}
	iog_cgraph_free(compact);
	free(blob);
}

static void test_compact_preserves_range_edge(void)
{
	static const u8 hit[] = "a5";
	static const u8 miss[] = "aX";
	struct range_blob {
		struct iog_blob_hdr hdr;
		struct iog_node nodes[3];
		struct iog_edge edges[2];
		struct iog_entry entries[1];
		struct iog_accept accepts[2];
	} blob = {
		.hdr = {
			.magic = IOG_MAGIC,
			.version = IOG_VERSION,
			.node_cnt = 3,
			.edge_cnt = 2,
			.entry_cnt = 1,
			.accept_cnt = 2,
			.alphabet_size = 256,
			.initial_state = 0,
			.nodes_off = offsetof(struct range_blob, nodes),
			.edges_off = offsetof(struct range_blob, edges),
			.entries_off = offsetof(struct range_blob, entries),
			.accepts_off = offsetof(struct range_blob, accepts),
			.total_size = sizeof(struct range_blob),
			.max_input_len = 16,
		},
		.nodes = {
			{ .edge_start = 0, .edge_cnt = 1,
			  .default_dst = IOG_NO_STATE },
			{ .edge_start = 1, .edge_cnt = 1,
			  .default_dst = IOG_NO_STATE },
			{ .edge_start = 2, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 1 },
		},
		.edges = {
			{ .sym_lo = 'a', .sym_hi = 'a', .dst = 1 },
			{ .sym_lo = '0', .sym_hi = '9', .dst = 2 },
		},
		.entries = {
			{ .id = 0, .state = 0 },
		},
		.accepts = {
			{ .id = 0, .code = 0 },
			{ .id = 1, .code = 7 },
		},
	};
	struct iog_cgraph *compact;
	struct iog_graph graph;
	char err[256] = "";

	TEST_ASSERT(!iog_graph_from_blob(&graph, &blob, sizeof(blob),
					 &iog_default_limits, err,
					 sizeof(err)));
	TEST_ASSERT(iog_run_action(&graph, hit, sizeof(hit) - 1) == 7);
	TEST_ASSERT(!iog_graph_inline_accept_codes(&graph));
	TEST_ASSERT(!iog_cgraph_new(&graph, &compact));
	TEST_ASSERT(iog_cgraph_run_action(compact, hit, sizeof(hit) - 1) == 7);
	TEST_ASSERT(iog_cgraph_run_action(compact, miss, sizeof(miss) - 1) == 0);
	TEST_ASSERT(iog_cgraph_count_transitions(compact, hit,
						 sizeof(hit) - 1) == 2);
	iog_cgraph_free(compact);
}

static void test_compact_preserves_longest_accept(void)
{
	static const u8 short_prefix[] = "/drop/";
	static const u8 long_prefix[] = "/drop/event";
	static const struct iog_prefix prefixes[] = {
		{ .bytes = short_prefix, .len = sizeof(short_prefix) - 1,
		  .action_code = 1 },
		{ .bytes = long_prefix, .len = sizeof(long_prefix) - 1,
		  .action_code = 2 },
	};
	static const u8 s0[] = "/drop/";
	static const u8 s1[] = "/drop/event";
	static const u8 s2[] = "/drop/event/child";
	static const u8 s3[] = "/drop/other";
	static const u8 s4[] = "/drip/";
	static const struct sample_bytes samples[] = {
		{ s0, sizeof(s0) - 1 },
		{ s1, sizeof(s1) - 1 },
		{ s2, sizeof(s2) - 1 },
		{ s3, sizeof(s3) - 1 },
		{ s4, sizeof(s4) - 1 },
	};

	check_compact_equiv(prefixes, 2, samples,
			    sizeof(samples) / sizeof(samples[0]));
}

static void test_compact_high_fanout_dispatch(void)
{
	struct fanout_blob {
		struct iog_blob_hdr hdr;
		struct iog_node nodes[17];
		struct iog_edge edges[16];
		struct iog_entry entries[1];
		struct iog_accept accepts[17];
	} blob = {
		.hdr = {
			.magic = IOG_MAGIC,
			.version = IOG_VERSION,
			.node_cnt = 17,
			.edge_cnt = 16,
			.entry_cnt = 1,
			.accept_cnt = 17,
			.alphabet_size = 256,
			.initial_state = 0,
			.nodes_off = offsetof(struct fanout_blob, nodes),
			.edges_off = offsetof(struct fanout_blob, edges),
			.entries_off = offsetof(struct fanout_blob, entries),
			.accepts_off = offsetof(struct fanout_blob, accepts),
			.total_size = sizeof(struct fanout_blob),
			.max_input_len = 16,
		},
		.nodes = {
			{ .edge_start = 0, .edge_cnt = 16,
			  .default_dst = IOG_NO_STATE },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 1 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 2 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 3 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 4 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 5 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 6 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 7 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 8 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 9 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 10 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 11 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 12 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 13 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 14 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 15 },
			{ .edge_start = 16, .edge_cnt = 0,
			  .default_dst = IOG_NO_STATE, .accept_id = 16 },
		},
		.entries = {
			{ .id = 0, .state = 0 },
		},
	};
	struct iog_cgraph *compact;
	struct iog_cgraph_stats stats;
	struct iog_graph graph;
	char err[256] = "";
	u32 i;

	for (i = 0; i < 16; i++) {
		blob.edges[i].sym_lo = 'a' + i;
		blob.edges[i].sym_hi = 'a' + i;
		blob.edges[i].dst = i + 1;
		blob.accepts[i + 1].id = i + 1;
		blob.accepts[i + 1].code = 100 + i;
	}

	TEST_ASSERT(!iog_graph_from_blob(&graph, &blob, sizeof(blob),
					 &iog_default_limits, err,
					 sizeof(err)));
	TEST_ASSERT(!iog_graph_inline_accept_codes(&graph));
	TEST_ASSERT(!iog_cgraph_new(&graph, &compact));
	TEST_ASSERT(!iog_cgraph_stats(compact, &stats));
	TEST_ASSERT(stats.dispatch_tables == 1);
	TEST_ASSERT(iog_cgraph_run_action(compact, (const u8 *)"a", 1) == 100);
	TEST_ASSERT(iog_cgraph_run_action(compact, (const u8 *)"p", 1) == 115);
	TEST_ASSERT(iog_cgraph_run_action(compact, (const u8 *)"z", 1) == 0);
	TEST_ASSERT(iog_cgraph_run_action_idx(compact, (const u8 *)"c", 1, 0) ==
		    102);
	TEST_ASSERT(iog_cgraph_run_action_idx(compact, (const u8 *)"c", 1, 1) ==
		    0);
	iog_cgraph_free(compact);
}

static void test_compact_shared_and_long_paths(void)
{
	static const u8 common[] =
		"/srv/shared/tenant/default/namespace/prod/runtime/";
	struct iog_prefix prefixes[32];
	struct sample_bytes samples[8];
	u8 storage[32][96];
	u8 sample_storage[8][112];
	size_t i;

	for (i = 0; i < 32; i++) {
		int len = snprintf((char *)storage[i], sizeof(storage[i]),
				   "%sservice-%02zu/component-%02zu/bin",
				   (const char *)common, i, i % 7);

		TEST_ASSERT(len > 0 && (size_t)len < sizeof(storage[i]));
		prefixes[i].bytes = storage[i];
		prefixes[i].len = (u32)len;
		prefixes[i].action_code = (u32)(i + 1);
	}

	for (i = 0; i < 4; i++) {
		int len = snprintf((char *)sample_storage[i],
				   sizeof(sample_storage[i]), "%s",
				   (const char *)storage[i * 7]);

		TEST_ASSERT(len > 0 &&
			    (size_t)len < sizeof(sample_storage[i]));
		samples[i].bytes = sample_storage[i];
		samples[i].len = (u32)len;
	}
	for (i = 4; i < 7; i++) {
		int len = snprintf((char *)sample_storage[i],
				   sizeof(sample_storage[i]), "%s/suffix-%zu",
				   (const char *)storage[(i - 4) * 9], i);

		TEST_ASSERT(len > 0 &&
			    (size_t)len < sizeof(sample_storage[i]));
		samples[i].bytes = sample_storage[i];
		samples[i].len = (u32)len;
	}
	samples[7].bytes = (const u8 *)"/srv/shared/tenant/miss";
	samples[7].len = sizeof("/srv/shared/tenant/miss") - 1;

	check_compact_equiv(prefixes, 32, samples,
			    sizeof(samples) / sizeof(samples[0]));
}

static void test_compact_generated_prefix_set(void)
{
	struct iog_prefix prefixes[64];
	struct sample_bytes samples[10];
	u8 storage[64][40];
	u8 sample_storage[10][56];
	u32 x = 0x12345678u;
	size_t i;

	for (i = 0; i < 64; i++) {
		int len;

		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		len = snprintf((char *)storage[i], sizeof(storage[i]),
			       "/r/%02zu/%08x/%02x", i, x, x & 0xffu);
		TEST_ASSERT(len > 0 && (size_t)len < sizeof(storage[i]));
		prefixes[i].bytes = storage[i];
		prefixes[i].len = (u32)len;
		prefixes[i].action_code = (u32)(100 + i);
	}

	for (i = 0; i < 5; i++) {
		int len = snprintf((char *)sample_storage[i],
				   sizeof(sample_storage[i]), "%s",
				   (const char *)storage[i * 11]);

		TEST_ASSERT(len > 0 &&
			    (size_t)len < sizeof(sample_storage[i]));
		samples[i].bytes = sample_storage[i];
		samples[i].len = (u32)len;
	}
	for (i = 5; i < 9; i++) {
		int len = snprintf((char *)sample_storage[i],
				   sizeof(sample_storage[i]), "%s/tail",
				   (const char *)storage[(i - 5) * 13]);

		TEST_ASSERT(len > 0 &&
			    (size_t)len < sizeof(sample_storage[i]));
		samples[i].bytes = sample_storage[i];
		samples[i].len = (u32)len;
	}
	samples[9].bytes = (const u8 *)"/r/no-match";
	samples[9].len = sizeof("/r/no-match") - 1;

	check_compact_equiv(prefixes, 64, samples,
			    sizeof(samples) / sizeof(samples[0]));
}

static u32 test_xorshift32(u32 *state)
{
	u32 x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static void test_compact_random_prefix_differential(void)
{
	enum {
		ROUNDS = 16,
		PREFIX_NR = 96,
		SAMPLE_NR_LOCAL = 192,
		PREFIX_MAX = 72,
		SAMPLE_MAX = 96,
	};
	static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789-_.";
	struct iog_prefix prefixes[PREFIX_NR];
	struct sample_bytes samples[SAMPLE_NR_LOCAL];
	u8 prefix_storage[PREFIX_NR][PREFIX_MAX];
	u8 sample_storage[SAMPLE_NR_LOCAL][SAMPLE_MAX];
	size_t round;

	for (round = 0; round < ROUNDS; round++) {
		u32 seed = 0x9e3779b9u ^ (u32)(round * 0x85ebca6bu);
		size_t i;

		for (i = 0; i < PREFIX_NR; i++) {
			u32 len = 6 + test_xorshift32(&seed) % 58;
			u32 j;

			TEST_ASSERT(len < PREFIX_MAX);
			prefix_storage[i][0] = '/';
			for (j = 1; j < len; j++) {
				u32 x = test_xorshift32(&seed);

				prefix_storage[i][j] =
					(u8)alphabet[x % (sizeof(alphabet) - 1)];
			}
			prefixes[i].bytes = prefix_storage[i];
			prefixes[i].len = len;
			prefixes[i].action_code = 1000u + (u32)i +
						  (u32)round * 100u;
		}

		for (i = 0; i < SAMPLE_NR_LOCAL; i++) {
			u32 mode = test_xorshift32(&seed) % 4u;
			u32 chosen = test_xorshift32(&seed) % PREFIX_NR;
			u32 len = prefixes[chosen].len;

			TEST_ASSERT(len < SAMPLE_MAX);
			memcpy(sample_storage[i], prefixes[chosen].bytes, len);
			if (mode == 0) {
				/* exact hit */
			} else if (mode == 1) {
				/* hit with suffix; last-accept should survive */
				u32 extra = 1 + test_xorshift32(&seed) % 12u;
				u32 j;

				if (len + extra >= SAMPLE_MAX)
					extra = SAMPLE_MAX - len - 1;
				for (j = 0; j < extra; j++)
					sample_storage[i][len + j] =
						(u8)alphabet[test_xorshift32(&seed) %
							     (sizeof(alphabet) - 1)];
				len += extra;
			} else if (mode == 2 && len > 2) {
				/* late miss */
				u32 pos = 1 + test_xorshift32(&seed) % (len - 1);

				sample_storage[i][pos] ^= 0x40u;
			} else {
				/* unrelated early miss */
				len = 6 + test_xorshift32(&seed) % 32u;
				TEST_ASSERT(len < SAMPLE_MAX);
				sample_storage[i][0] = '!';
				for (u32 j = 1; j < len; j++)
					sample_storage[i][j] =
						(u8)alphabet[test_xorshift32(&seed) %
							     (sizeof(alphabet) - 1)];
			}
			samples[i].bytes = sample_storage[i];
			samples[i].len = len;
		}

		check_compact_equiv(prefixes, PREFIX_NR, samples,
				    SAMPLE_NR_LOCAL);
	}
}

int main(void)
{
	static const u8 path[] = "/drop/event";
	struct iog_run_result result;
	struct iog_cgraph *compact;
	struct iog_cgraph_stats cstats;
	struct iog_graph graph;
	char err[256] = "";
	void *blob;
	size_t blob_len;

	test_blob(&blob, &blob_len);
	TEST_ASSERT(!iog_graph_from_blob(&graph, blob, blob_len,
					 &iog_default_limits, err,
					 sizeof(err)));
	TEST_ASSERT(!iog_run(&graph, path, sizeof(path) - 1, 0, &result));
	TEST_ASSERT(result.action_code == 1);
	TEST_ASSERT(result.consumed == sizeof("/drop/") - 1);
	TEST_ASSERT(iog_run_action(&graph, path, sizeof(path) - 1) == 1);
	TEST_ASSERT(iog_run_action_entry(&graph, path, sizeof(path) - 1, 0) == 1);
	TEST_ASSERT(iog_run_first_action_entry(&graph, path,
					       sizeof(path) - 1, 0) == 1);
	TEST_ASSERT(!iog_graph_inline_accept_codes(&graph));
	TEST_ASSERT(!iog_cgraph_new(&graph, &compact));
	TEST_ASSERT(!iog_cgraph_stats(compact, &cstats));
	TEST_ASSERT(cstats.literal_edges >= 1);
	TEST_ASSERT(iog_cgraph_run_action(compact, path,
					  sizeof(path) - 1) == 1);
	TEST_ASSERT(iog_cgraph_count_transitions(compact, path,
						 sizeof(path) - 1) == 1);
	iog_cgraph_free(compact);
	test_compact_preserves_range_edge();
	test_compact_preserves_longest_accept();
	test_compact_high_fanout_dispatch();
	test_compact_shared_and_long_paths();
	test_compact_generated_prefix_set();
	test_compact_random_prefix_differential();
	free(blob);
	return 0;
}
