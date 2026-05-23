#include "bench_internal.h"

static size_t align8_size(size_t value)
{
	return (value + 7u) & ~(size_t)7u;
}

size_t event_record_bytes(u32 payload)
{
	return align8_size(sizeof(struct event_record) + payload);
}

size_t ringbuf_record_bytes(u32 payload)
{
	return align8_size(IOG_BENCH_RINGBUF_HDR_SZ_MODEL +
			   event_record_bytes(payload));
}

size_t postdrop_intermediate_bytes(u32 payload)
{
	return ringbuf_record_bytes(payload) + sizeof(struct decoded_event);
}
