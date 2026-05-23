#include "test_common.h"

int main(void)
{
	static const u8 path[] = "/drop/event";
	struct iog_run_result result;
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
	free(blob);
	return 0;
}
