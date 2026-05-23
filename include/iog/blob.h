#ifndef IOG_BLOB_H
#define IOG_BLOB_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define IOG_MAGIC		0x494f4752u /* "IOGR" */
#define IOG_VERSION		1u
#define IOG_NO_STATE		UINT32_MAX
#define IOG_MAX_DEFAULT_DEPTH	8u

struct iog_blob_hdr {
	u32 magic;
	u16 version;
	u16 flags;
	u32 node_cnt;
	u32 edge_cnt;
	u32 entry_cnt;
	u32 accept_cnt;
	u32 alphabet_size;
	u32 initial_state;
	u32 nodes_off;
	u32 edges_off;
	u32 entries_off;
	u32 accepts_off;
	u32 total_size;
	u32 max_input_len;
	u32 reserved;
};

struct iog_node {
	u32 edge_start;
	u16 edge_cnt;
	u16 flags;
	u32 default_dst;
	u32 accept_id;
};

struct iog_edge {
	u32 sym_lo;
	u32 sym_hi;
	u32 dst;
};

struct iog_entry {
	u32 id;
	u32 state;
};

struct iog_accept {
	u32 id;
	u32 code;
};

struct iog_limits {
	u32 max_nodes;
	u32 max_edges;
	u32 max_entries;
	u32 max_accepts;
	u32 max_alphabet;
	u32 max_input_len;
};

extern const struct iog_limits iog_default_limits;

#endif
