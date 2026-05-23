#ifndef IOG_INTERP_H
#define IOG_INTERP_H

#include <iog/graph.h>

struct iog_run_result {
	u32 final_state;
	u32 action_code;
	u32 consumed;
};

u32 iog_run_action_entry(const struct iog_graph *graph, const u8 *buf,
			 u32 len, u32 entry_id);
u32 iog_run_first_action_entry(const struct iog_graph *graph, const u8 *buf,
			       u32 len, u32 entry_id);
u32 iog_run_action(const struct iog_graph *graph, const u8 *buf, u32 len);
int iog_run(const struct iog_graph *graph, const u8 *buf, u32 len,
	    u32 entry_id, struct iog_run_result *result);
int iog_graph_step(const struct iog_graph *graph, u32 state, u32 sym,
		   u32 *next_state, u32 *action_code);

#endif
