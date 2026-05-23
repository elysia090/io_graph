#ifndef IOG_COMPILE_H
#define IOG_COMPILE_H

#include <iog/blob.h>

struct iog_prefix {
	const u8 *bytes;
	u32 len;
	u32 action_code;
};

struct iog_compile_stats {
	u32 node_cnt;
	u32 edge_cnt;
	u32 accept_cnt;
	u64 prefix_bytes;
	u64 blob_bytes;
	u64 dense_table_bytes;
	u64 gen_chain_source_bytes;
	u64 gen_chain_bpf_bytes;
	u64 list_payload_bytes;
};

int iog_compile_prefixes(const struct iog_prefix *prefixes, size_t nr,
			 u32 max_input_len, void **blob_out,
			 size_t *blob_len_out,
			 struct iog_compile_stats *stats,
			 char *err, size_t err_len);
u64 iog_dense_table_bytes(u32 node_cnt);
u64 iog_generated_chain_source_bytes(const struct iog_prefix *prefixes,
				     size_t nr);
u64 iog_generated_chain_bpf_bytes(const struct iog_prefix *prefixes,
				  size_t nr);
u64 iog_list_payload_bytes(const struct iog_prefix *prefixes, size_t nr);

#endif
