#ifndef IOG_VERIFY_H
#define IOG_VERIFY_H

#include <iog/blob.h>

int iog_verify_blob(const void *blob, size_t len,
		    const struct iog_limits *limits,
		    char *err, size_t err_len);

#endif
