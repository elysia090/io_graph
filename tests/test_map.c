#include "test_common.h"

int main(void)
{
	static const u8 path[] = "/drop/event";
	struct iog_map map;
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
	free(blob);
	return 0;
}
