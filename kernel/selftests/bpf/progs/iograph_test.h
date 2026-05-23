/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __IOGRAPH_TEST_H
#define __IOGRAPH_TEST_H

#include <linux/bpf_iograph.h>

#define IOGRAPH_TEST_PATH	"/drop/event"
#define IOGRAPH_TEST_NODES	7
#define IOGRAPH_TEST_EDGES	6
#define IOGRAPH_TEST_ENTRIES	1
#define IOGRAPH_TEST_ACCEPTS	2

enum iograph_test_action {
	IOGRAPH_TEST_NOMATCH = 0,
	IOGRAPH_TEST_DROP = 1,
};

struct iograph_test_blob {
	struct iog_blob_hdr hdr;
	struct iog_node nodes[IOGRAPH_TEST_NODES];
	struct iog_edge edges[IOGRAPH_TEST_EDGES];
	struct iog_entry entries[IOGRAPH_TEST_ENTRIES];
	struct iog_accept accepts[IOGRAPH_TEST_ACCEPTS];
};

struct iograph_test_result {
	__u32 kfunc_ret;
	__u32 final_state;
	__u32 action_code;
	__u32 ringbuf_reserve_seen;
};

#endif /* __IOGRAPH_TEST_H */
