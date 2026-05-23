#ifndef IOG_INTERNAL_H
#define IOG_INTERNAL_H

#include <iog/bench.h>
#include <iog/verify.h>

#include <stdbool.h>
#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)
#else
#define likely(x)	(x)
#define unlikely(x)	(x)
#endif

void iog_set_err(char *err, size_t err_len, const char *fmt, ...);
bool iog_add_overflow_size(size_t a, size_t b, size_t *out);
bool iog_mul_overflow_size(size_t a, size_t b, size_t *out);
bool iog_section_fits(size_t len, u32 off, u32 cnt, size_t elem_size);

#endif
