#include "test_common.h"

int main(void)
{
	struct iog_blob_hdr *hdr;
	struct iog_node *nodes;
	char err[256] = "";
	void *blob;
	size_t blob_len;

	test_blob(&blob, &blob_len);
	hdr = blob;
	hdr->magic ^= 1;
	TEST_ASSERT(iog_verify_blob(blob, blob_len, &iog_default_limits, err,
				    sizeof(err)) != 0);
	free(blob);

	test_blob(&blob, &blob_len);
	hdr = blob;
	TEST_ASSERT(iog_verify_blob(blob, blob_len + 1, &iog_default_limits,
				    err, sizeof(err)) != 0);
	free(blob);

	test_blob(&blob, &blob_len);
	hdr = blob;
	blob = realloc(blob, blob_len + 4);
	TEST_ASSERT(blob);
	hdr = blob;
	memset((u8 *)blob + blob_len, 0, 4);
	hdr->total_size = (u32)blob_len + 4;
	TEST_ASSERT(iog_verify_blob(blob, blob_len + 4, &iog_default_limits,
				    err, sizeof(err)) != 0);
	free(blob);

	test_blob(&blob, &blob_len);
	hdr = blob;
	nodes = (void *)((u8 *)blob + hdr->nodes_off);
	nodes[0].default_dst = 0;
	TEST_ASSERT(!iog_verify_blob(blob, blob_len, &iog_default_limits, err,
				     sizeof(err)));
	free(blob);

	test_blob(&blob, &blob_len);
	hdr = blob;
	nodes = (void *)((u8 *)blob + hdr->nodes_off);
	nodes[0].flags = IOG_NODE_F_FINAL_ACTION;
	TEST_ASSERT(iog_verify_blob(blob, blob_len, &iog_default_limits, err,
				    sizeof(err)) != 0);
	free(blob);
	return 0;
}
