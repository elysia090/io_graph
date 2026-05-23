#ifndef IOG_BENCH_H
#define IOG_BENCH_H

#include <iog/compile.h>
#include <iog/interp.h>
#include <iog/map.h>
#include <iog/verify.h>

u32 iog_generated_chain_match(const struct iog_prefix *prefixes, size_t nr,
			      const u8 *buf, u32 len);
u32 iog_list_match(const struct iog_prefix *prefixes, size_t nr,
		   const u8 *buf, u32 len);

#endif
