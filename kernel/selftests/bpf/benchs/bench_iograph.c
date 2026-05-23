// SPDX-License-Identifier: GPL-2.0
#include <argp.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "bench.h"
#include "iograph_bench.skel.h"

#define IOGRAPH_DEFAULT_SELECTOR	"/drop/event"
#define IOGRAPH_BENCH_SELECTOR_CAP	256
#define IOGRAPH_TRIGGER_BATCH		1024

struct iograph_lpm_key {
	u32 prefixlen;
	unsigned char data[IOGRAPH_BENCH_SELECTOR_CAP];
};

static struct iograph_ctx {
	struct iograph_bench *skel;
	struct bpf_link *link;
	struct ring_buffer *ringbuf;
	void *blob;
	size_t blob_len;
	struct counter *triggers;
} ctx;

static struct iograph_args {
	const char *blob_path;
	const char *prefixes_path;
	const char *selector;
	u32 drop_action;
} args = {
	.selector = IOGRAPH_DEFAULT_SELECTOR,
	.drop_action = 1,
};

enum {
	ARG_BLOB = 7000,
	ARG_PREFIXES,
	ARG_SELECTOR,
	ARG_DROP_ACTION,
};

static const struct argp_option opts[] = {
	{ "blob", ARG_BLOB, "BLOB", 0, "Verified io_graph blob path" },
	{ "prefixes", ARG_PREFIXES, "TXT", 0, "Prefix lines for LPM trie" },
	{ "selector", ARG_SELECTOR, "BYTES", 0, "Raw selector bytes" },
	{ "drop-action", ARG_DROP_ACTION, "CODE", 0, "DROP action code" },
	{},
};

static error_t iograph_parse_arg(int key, char *arg, struct argp_state *state)
{
	char *end = NULL;
	unsigned long code;

	switch (key) {
	case ARG_BLOB:
		args.blob_path = arg;
		break;
	case ARG_PREFIXES:
		args.prefixes_path = arg;
		break;
	case ARG_SELECTOR:
		args.selector = arg;
		break;
	case ARG_DROP_ACTION:
		code = strtoul(arg, &end, 0);
		if (!end || *end || code > UINT_MAX)
			argp_usage(state);
		args.drop_action = code;
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}

	return 0;
}

const struct argp bench_iograph_argp = {
	.options = opts,
	.parser = iograph_parse_arg,
};

static void die_errno(const char *what, int err)
{
	fprintf(stderr, "%s: %s\n", what, strerror(err < 0 ? -err : err));
	exit(1);
}

static void read_blob(void)
{
	long file_len;
	FILE *fp;

	if (!args.blob_path) {
		fprintf(stderr, "iograph benchmark requires --blob\n");
		exit(1);
	}

	fp = fopen(args.blob_path, "rb");
	if (!fp)
		die_errno("open blob", errno);
	if (fseek(fp, 0, SEEK_END))
		die_errno("seek blob end", errno);
	file_len = ftell(fp);
	if (file_len <= 0 || (unsigned long)file_len > UINT_MAX)
		die_errno("blob size", file_len < 0 ? errno : E2BIG);
	if (fseek(fp, 0, SEEK_SET))
		die_errno("seek blob start", errno);

	ctx.blob = malloc(file_len);
	if (!ctx.blob)
		die_errno("alloc blob", ENOMEM);
	ctx.blob_len = file_len;
	if (fread(ctx.blob, 1, ctx.blob_len, fp) != ctx.blob_len)
		die_errno("read blob", ferror(fp) ? EIO : EINVAL);
	if (fclose(fp))
		die_errno("close blob", errno);
}

static int event_cb(void *opaque, void *data, size_t len)
{
	(void)opaque;
	(void)data;
	(void)len;
	return 0;
}

static void iograph_validate_common(void)
{
	if (env.consumer_cnt > 1) {
		fprintf(stderr, "iograph benchmark supports at most one consumer\n");
		exit(1);
	}
	if (strlen(args.selector) >= IOGRAPH_BENCH_SELECTOR_CAP) {
		fprintf(stderr, "selector exceeds %u bytes\n",
			IOGRAPH_BENCH_SELECTOR_CAP - 1);
		exit(1);
	}
}

static void iograph_validate(void)
{
	iograph_validate_common();
	if (!args.blob_path) {
		fprintf(stderr, "iograph benchmark requires --blob\n");
		exit(1);
	}
}

static void iograph_lpm_validate(void)
{
	iograph_validate_common();
	if (!args.prefixes_path) {
		fprintf(stderr, "iograph LPM benchmark requires --prefixes\n");
		exit(1);
	}
}

static void iograph_decision_validate(void)
{
	iograph_validate();
	if (env.consumer_cnt) {
		fprintf(stderr, "iograph decision benchmark does not emit events\n");
		exit(1);
	}
}

static void iograph_lpm_decision_validate(void)
{
	iograph_lpm_validate();
	if (env.consumer_cnt) {
		fprintf(stderr, "iograph LPM decision benchmark does not emit events\n");
		exit(1);
	}
}

static void iograph_hook_floor_validate(void)
{
	iograph_validate_common();
	if (env.consumer_cnt) {
		fprintf(stderr, "iograph hook floor benchmark does not emit events\n");
		exit(1);
	}
}

static void update_lpm_policy(void)
{
	struct iograph_lpm_key key;
	FILE *fp;
	char line[IOGRAPH_BENCH_SELECTOR_CAP + 4];
	size_t nr = 0;
	int fd;

	fp = fopen(args.prefixes_path, "r");
	if (!fp)
		die_errno("open LPM prefixes", errno);
	fd = bpf_map__fd(ctx.skel->maps.lpm_policy);
	while (fgets(line, sizeof(line), fp)) {
		size_t len = strcspn(line, "\r\n");
		u32 action = 1;

		if (!len || line[0] == '#')
			continue;
		if (len >= sizeof(key.data)) {
			fprintf(stderr, "LPM prefix exceeds %u bytes\n",
				IOGRAPH_BENCH_SELECTOR_CAP - 1);
			exit(1);
		}
		memset(&key, 0, sizeof(key));
		key.prefixlen = len * 8u;
		memcpy(key.data, line, len);
		if (bpf_map_update_elem(fd, &key, &action, BPF_ANY))
			die_errno("update LPM policy", errno);
		nr++;
	}
	if (ferror(fp))
		die_errno("read LPM prefixes", EIO);
	if (fclose(fp))
		die_errno("close LPM prefixes", errno);
	if (!nr) {
		fprintf(stderr, "LPM prefix file has no policy rows\n");
		exit(1);
	}
	printf("iograph_lpm_prefixes=%zu\n", nr);
}

static void iograph_alloc_triggers(void)
{
	ctx.triggers = calloc(env.producer_cnt, sizeof(*ctx.triggers));
	if (!ctx.triggers)
		die_errno("alloc trigger counters", ENOMEM);
}

static long iograph_collect_triggers(void)
{
	long triggers = 0;
	int i;

	for (i = 0; i < env.producer_cnt; i++)
		triggers += atomic_swap(&ctx.triggers[i].value, 0);
	return triggers;
}

static void iograph_setup_common(bool use_lpm, bool decision_only,
				 bool lpm_bounded_copy)
{
	size_t selector_len = strlen(args.selector);
	struct bpf_program *prog;
	u32 key = 0;
	int err;

	setup_libbpf();
	if (!use_lpm)
		read_blob();

	ctx.skel = iograph_bench__open();
	if (!ctx.skel) {
		fprintf(stderr, "failed to open iograph skeleton\n");
		exit(1);
	}

	if (!use_lpm) {
		err = bpf_map__set_value_size(ctx.skel->maps.policy,
					      ctx.blob_len);
		if (err)
			die_errno("set policy value size", err);
	}

	ctx.skel->rodata->selector_len = selector_len;
	ctx.skel->rodata->drop_action = args.drop_action;
	memcpy(ctx.skel->bss->selector, args.selector, selector_len);

	err = iograph_bench__load(ctx.skel);
	if (err)
		die_errno("load iograph skeleton", err);

	if (use_lpm) {
		update_lpm_policy();
		if (lpm_bounded_copy) {
			prog = decision_only ?
				ctx.skel->progs.iograph_lpm_bounded_decision_bench_run :
				ctx.skel->progs.iograph_lpm_bounded_bench_run;
		} else {
			prog = decision_only ?
				ctx.skel->progs.iograph_lpm_decision_bench_run :
				ctx.skel->progs.iograph_lpm_bench_run;
		}
	} else {
		err = bpf_map_update_elem(bpf_map__fd(ctx.skel->maps.policy),
					  &key, ctx.blob, BPF_ANY);
		if (err)
			die_errno("update iograph policy", errno);
		prog = decision_only ?
			ctx.skel->progs.iograph_decision_bench_run :
			ctx.skel->progs.iograph_bench_run;
	}
	iograph_alloc_triggers();

	ctx.link = bpf_program__attach(prog);
	err = libbpf_get_error(ctx.link);
	if (err)
		die_errno("attach iograph tracing program", err);

	if (!env.consumer_cnt)
		return;

	ctx.ringbuf = ring_buffer__new(bpf_map__fd(ctx.skel->maps.events),
				       event_cb, NULL, NULL);
	err = libbpf_get_error(ctx.ringbuf);
	if (err) {
		ctx.ringbuf = NULL;
		die_errno("open iograph ringbuf", err);
	}
}

static void iograph_setup(void)
{
	iograph_setup_common(false, false, false);
}

static void iograph_lpm_setup(void)
{
	iograph_setup_common(true, false, false);
}

static void iograph_lpm_bounded_setup(void)
{
	iograph_setup_common(true, false, true);
}

static void iograph_decision_setup(void)
{
	iograph_setup_common(false, true, false);
}

static void iograph_lpm_decision_setup(void)
{
	iograph_setup_common(true, true, false);
}

static void iograph_lpm_bounded_decision_setup(void)
{
	iograph_setup_common(true, true, true);
}

static void iograph_hook_floor_setup(void)
{
	int err;

	setup_libbpf();
	ctx.skel = iograph_bench__open();
	if (!ctx.skel) {
		fprintf(stderr, "failed to open iograph skeleton\n");
		exit(1);
	}

	err = iograph_bench__load(ctx.skel);
	if (err)
		die_errno("load iograph skeleton", err);

	iograph_alloc_triggers();
	ctx.link = bpf_program__attach(ctx.skel->progs.iograph_hook_floor_bench_run);
	err = libbpf_get_error(ctx.link);
	if (err)
		die_errno("attach iograph hook floor program", err);
}

static void *iograph_producer(void *opaque)
{
	long producer = (long)opaque;
	long triggers = 0;

	while (true) {
		(void)syscall(__NR_getpgid);
		triggers++;
		if (triggers == IOGRAPH_TRIGGER_BATCH) {
			atomic_add(&ctx.triggers[producer].value, triggers);
			triggers = 0;
		}
	}
	return NULL;
}

static void *iograph_consumer(void *opaque)
{
	(void)opaque;
	while (true)
		(void)ring_buffer__poll(ctx.ringbuf, 100);
	return NULL;
}

static void iograph_measure(struct bench_res *res)
{
	long triggers = iograph_collect_triggers();

	res->hits = atomic_swap(&ctx.skel->bss->posts, 0);
	res->drops = triggers > res->hits ? triggers - res->hits : 0;
}

static void iograph_decision_measure(struct bench_res *res)
{
	res->hits = iograph_collect_triggers();
}

const struct bench bench_iograph_prefilter = {
	.name = "iograph-prefilter",
	.argp = &bench_iograph_argp,
	.validate = iograph_validate,
	.setup = iograph_setup,
	.producer_thread = iograph_producer,
	.consumer_thread = iograph_consumer,
	.measure = iograph_measure,
	.report_progress = hits_drops_report_progress,
	.report_final = hits_drops_report_final,
};

const struct bench bench_iograph_compact_prefilter = {
	.name = "iograph-compact-prefilter",
	.argp = &bench_iograph_argp,
	.validate = iograph_validate,
	.setup = iograph_setup,
	.producer_thread = iograph_producer,
	.consumer_thread = iograph_consumer,
	.measure = iograph_measure,
	.report_progress = hits_drops_report_progress,
	.report_final = hits_drops_report_final,
};

const struct bench bench_iograph_lpm_prefilter = {
	.name = "iograph-lpm-prefilter",
	.argp = &bench_iograph_argp,
	.validate = iograph_lpm_validate,
	.setup = iograph_lpm_setup,
	.producer_thread = iograph_producer,
	.consumer_thread = iograph_consumer,
	.measure = iograph_measure,
	.report_progress = hits_drops_report_progress,
	.report_final = hits_drops_report_final,
};

const struct bench bench_iograph_lpm_bounded_prefilter = {
	.name = "iograph-lpm-bounded-prefilter",
	.argp = &bench_iograph_argp,
	.validate = iograph_lpm_validate,
	.setup = iograph_lpm_bounded_setup,
	.producer_thread = iograph_producer,
	.consumer_thread = iograph_consumer,
	.measure = iograph_measure,
	.report_progress = hits_drops_report_progress,
	.report_final = hits_drops_report_final,
};

const struct bench bench_iograph_decision = {
	.name = "iograph-decision",
	.argp = &bench_iograph_argp,
	.validate = iograph_decision_validate,
	.setup = iograph_decision_setup,
	.producer_thread = iograph_producer,
	.measure = iograph_decision_measure,
	.report_progress = hits_drops_report_progress,
	.report_final = hits_drops_report_final,
};

const struct bench bench_iograph_compact_decision = {
	.name = "iograph-compact-decision",
	.argp = &bench_iograph_argp,
	.validate = iograph_decision_validate,
	.setup = iograph_decision_setup,
	.producer_thread = iograph_producer,
	.measure = iograph_decision_measure,
	.report_progress = hits_drops_report_progress,
	.report_final = hits_drops_report_final,
};

const struct bench bench_iograph_hook_floor = {
	.name = "iograph-hook-floor",
	.argp = &bench_iograph_argp,
	.validate = iograph_hook_floor_validate,
	.setup = iograph_hook_floor_setup,
	.producer_thread = iograph_producer,
	.measure = iograph_decision_measure,
	.report_progress = hits_drops_report_progress,
	.report_final = hits_drops_report_final,
};

const struct bench bench_iograph_lpm_decision = {
	.name = "iograph-lpm-decision",
	.argp = &bench_iograph_argp,
	.validate = iograph_lpm_decision_validate,
	.setup = iograph_lpm_decision_setup,
	.producer_thread = iograph_producer,
	.measure = iograph_decision_measure,
	.report_progress = hits_drops_report_progress,
	.report_final = hits_drops_report_final,
};

const struct bench bench_iograph_lpm_bounded_decision = {
	.name = "iograph-lpm-bounded-decision",
	.argp = &bench_iograph_argp,
	.validate = iograph_lpm_decision_validate,
	.setup = iograph_lpm_bounded_decision_setup,
	.producer_thread = iograph_producer,
	.measure = iograph_decision_measure,
	.report_progress = hits_drops_report_progress,
	.report_final = hits_drops_report_final,
};
