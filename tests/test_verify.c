#include "test_common.h"

int main(void)
{
	char err[256] = "";
	void *blob;
	size_t blob_len;

	test_blob(&blob, &blob_len);
	TEST_ASSERT(!iog_verify_blob(blob, blob_len, &iog_default_limits, err,
				     sizeof(err)));
	free(blob);
	return 0;
}
