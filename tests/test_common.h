#ifndef IOG_TEST_COMMON_H
#define IOG_TEST_COMMON_H

#include <iog/bench.h>
#include <iog/compact.h>
#include <iog/verify.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ASSERT(cond)						\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "%s:%d: %s\n", __FILE__,	\
				__LINE__, #cond);			\
			exit(1);					\
		}							\
	} while (0)

static inline void test_blob(void **blob, size_t *blob_len)
{
	static const u8 prefix[] = "/drop/";
	const struct iog_prefix policy = {
		.bytes = prefix,
		.len = sizeof(prefix) - 1,
		.action_code = 1,
	};
	char err[256] = "";
	int ret;

	ret = iog_compile_prefixes(&policy, 1, 64, blob, blob_len, NULL, err,
				   sizeof(err));
	if (ret) {
		fprintf(stderr, "compile failed: %s\n", err);
		exit(1);
	}
}

#endif
