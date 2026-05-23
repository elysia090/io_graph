#include "test_common.h"

int main(void)
{
	struct iog_blob_hdr *hdr;
	char err[256] = "";
	void *blob;
	size_t blob_len;

	test_blob(&blob, &blob_len);
	hdr = blob;
	hdr->magic ^= 1;
	TEST_ASSERT(iog_verify_blob(blob, blob_len, &iog_default_limits, err,
				    sizeof(err)) != 0);
	free(blob);
	return 0;
}
