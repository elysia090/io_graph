#include "iog_internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

_Static_assert(sizeof(struct iog_blob_hdr) == 60, "bad iog_blob_hdr size");
_Static_assert(sizeof(struct iog_node) == 16, "bad iog_node size");
_Static_assert(sizeof(struct iog_edge) == 12, "bad iog_edge size");
_Static_assert(sizeof(struct iog_entry) == 8, "bad iog_entry size");
_Static_assert(sizeof(struct iog_accept) == 8, "bad iog_accept size");

const struct iog_limits iog_default_limits = {
	.max_nodes = 1000000,
	.max_edges = 4000000,
	.max_entries = 64,
	.max_accepts = 65536,
	.max_alphabet = 256,
	.max_input_len = 65536,
};

void iog_set_err(char *err, size_t err_len, const char *fmt, ...)
{
	va_list ap;

	if (!err || !err_len)
		return;

	va_start(ap, fmt);
	vsnprintf(err, err_len, fmt, ap);
	va_end(ap);
}

bool iog_add_overflow_size(size_t a, size_t b, size_t *out)
{
	if (a > SIZE_MAX - b)
		return true;
	*out = a + b;
	return false;
}

bool iog_mul_overflow_size(size_t a, size_t b, size_t *out)
{
	if (a && b > SIZE_MAX / a)
		return true;
	*out = a * b;
	return false;
}

bool iog_section_fits(size_t len, u32 off, u32 cnt, size_t elem_size)
{
	size_t bytes, end;

	if (iog_mul_overflow_size(cnt, elem_size, &bytes))
		return false;
	if (off > len)
		return false;
	if (iog_add_overflow_size(off, bytes, &end))
		return false;

	return end <= len;
}
