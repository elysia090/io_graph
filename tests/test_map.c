#include "test_common.h"

int main(void)
{
	static const u8 path[] = "/drop/event";
	struct iog_map map;
	struct iog_bpf_map *bpf_map;
	struct iog_bpf_attr attr = {
		.key_size = IOG_BPF_KEY_SIZE,
		.max_entries = IOG_BPF_MAX_ENTRIES,
	};
	u32 key = 0;
	char err[256] = "";
	void *blob;
	size_t blob_len;

	test_blob(&blob, &blob_len);
	iog_map_init(&map);
	TEST_ASSERT(!iog_map_update_blob(&map, blob, blob_len,
					 &iog_default_limits, err,
					 sizeof(err)));
	TEST_ASSERT(iog_map_active_graph(&map)->accept_codes_inline);
	TEST_ASSERT(iog_map_run_action(&map, path, sizeof(path) - 1) == 1);
	TEST_ASSERT(iog_map_mem_usage(&map) >= blob_len);
	iog_map_destroy(&map);

	attr.value_size = (u32)blob_len + 16;
	TEST_ASSERT(!iog_bpf_map_alloc(&attr, &bpf_map));
	TEST_ASSERT(!iog_bpf_map_update_elem(bpf_map, &key, blob, IOG_BPF_ANY,
					     err, sizeof(err)));
	TEST_ASSERT(iog_bpf_kfunc_run_action(bpf_map, path,
					     sizeof(path) - 1, 0) == 1);
	iog_bpf_map_free(bpf_map);
	free(blob);
	return 0;
}
