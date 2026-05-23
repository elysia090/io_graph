#include "bench_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const char *roots[] = {
	"/var/lib/kubelet/pods/",
	"/var/log/containers/",
	"/opt/acme/cache/",
	"/srv/app/run/",
};

static const char *services[] = {
	"api", "worker", "sched", "ingest",
	"index", "proxy", "bill", "auth",
};

static void put_sample(struct sample *sample, const char *text)
{
	size_t len = strlen(text);

	if (len >= sizeof(sample->bytes)) {
		fprintf(stderr, "sample too long\n");
		exit(2);
	}

	memcpy(sample->bytes, text, len);
	sample->len = (u32)len;
}

static char *xstrdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *out = malloc(len);

	if (!out) {
		perror("malloc");
		exit(2);
	}
	memcpy(out, s, len);
	return out;
}

const char *workload_kind_name(enum workload_kind kind)
{
	switch (kind) {
	case WORKLOAD_TYPICAL:
		return "typical";
	case WORKLOAD_SHARED_PREFIX:
		return "shared-prefix";
	case WORKLOAD_LONG_PATH:
		return "long-path";
	}

	return "unknown";
}

static void make_typical_prefix(char *buf, size_t len, size_t i)
{
	snprintf(buf, len, "%st%02zu/n%02zu/%s/o%04zu/",
		 roots[i % ARRAY_SIZE(roots)],
		 i % 32,
		 (i / 32) % 8,
		 services[(i / 5) % ARRAY_SIZE(services)],
		 i % 1024);
}

static void make_typical_late_miss(char *buf, size_t len, size_t i)
{
	snprintf(buf, len, "%st%02zu/n%02zu/%s/not-present-%02zu",
		 roots[i % ARRAY_SIZE(roots)],
		 i % 32,
		 (i / 32) % 8,
		 services[(i / 5) % ARRAY_SIZE(services)],
		 i % 97);
}

static void make_shared_prefix(char *buf, size_t len, size_t i)
{
	snprintf(buf, len,
		 "/srv/shared/tenant/default/namespace/prod/runtime/"
		 "container/worker/shared/component/shard-%05zu/tail/",
		 i);
}

static void make_shared_late_miss(char *buf, size_t len, size_t i)
{
	snprintf(buf, len,
		 "/srv/shared/tenant/default/namespace/prod/runtime/"
		 "container/worker/shared/component/shard-%05zu/tail-miss",
		 i);
}

static void make_long_prefix(char *buf, size_t len, size_t i)
{
	snprintf(buf, len,
		 "/var/lib/kubelet/pods/tenant-long/namespace-long/"
		 "volume-subpaths/runtime/security/observability/"
		 "container-rootfs/application/component/args/"
		 "segment-a/segment-b/segment-c/segment-d/object-%05zu/",
		 i);
}

static void make_long_late_miss(char *buf, size_t len, size_t i)
{
	snprintf(buf, len,
		 "/var/lib/kubelet/pods/tenant-long/namespace-long/"
		 "volume-subpaths/runtime/security/observability/"
		 "container-rootfs/application/component/args/"
		 "segment-a/segment-b/segment-c/segment-d/object-%05zu-miss",
		 i);
}

static void make_prefix(char *buf, size_t len, size_t i,
			enum workload_kind kind)
{
	switch (kind) {
	case WORKLOAD_TYPICAL:
		make_typical_prefix(buf, len, i);
		break;
	case WORKLOAD_SHARED_PREFIX:
		make_shared_prefix(buf, len, i);
		break;
	case WORKLOAD_LONG_PATH:
		make_long_prefix(buf, len, i);
		break;
	}
}

static void make_late_miss(char *buf, size_t len, size_t i,
			   enum workload_kind kind)
{
	switch (kind) {
	case WORKLOAD_TYPICAL:
		make_typical_late_miss(buf, len, i);
		break;
	case WORKLOAD_SHARED_PREFIX:
		make_shared_late_miss(buf, len, i);
		break;
	case WORKLOAD_LONG_PATH:
		make_long_late_miss(buf, len, i);
		break;
	}
}

void workload_init(struct workload *wl, size_t nr, enum workload_kind kind)
{
	char buf[256];
	size_t i;

	memset(wl, 0, sizeof(*wl));
	wl->prefixes = calloc(nr, sizeof(*wl->prefixes));
	wl->owned_prefixes = calloc(nr, sizeof(*wl->owned_prefixes));
	if (!wl->prefixes || !wl->owned_prefixes) {
		perror("calloc");
		exit(2);
	}

	wl->nr = nr;
	for (i = 0; i < nr; i++) {
		make_prefix(buf, sizeof(buf), i, kind);
		wl->owned_prefixes[i] = (u8 *)xstrdup(buf);
		wl->prefixes[i].bytes = wl->owned_prefixes[i];
		wl->prefixes[i].len = (u32)strlen(buf);
		wl->prefixes[i].action_code = 1;
		wl->prefix_bytes += wl->prefixes[i].len;
	}

	for (i = 0; i < IOG_BENCH_SAMPLE_NR; i++) {
		size_t idx = nr == 1 ? 0 : i * (nr - 1) /
			     (IOG_BENCH_SAMPLE_NR - 1);

		snprintf(buf, sizeof(buf), "/x/miss/early-%02zu/not-matched", i);
		put_sample(&wl->early[i], buf);

		make_late_miss(buf, sizeof(buf), idx, kind);
		put_sample(&wl->late[i], buf);

		make_prefix(buf, sizeof(buf), idx, kind);
		put_sample(&wl->exact[i], buf);
		strncat(buf, "event.log", sizeof(buf) - strlen(buf) - 1);
		put_sample(&wl->hit[i], buf);
	}
}

void workload_free(struct workload *wl)
{
	size_t i;

	for (i = 0; i < wl->nr; i++)
		free(wl->owned_prefixes[i]);
	free(wl->owned_prefixes);
	free(wl->prefixes);
}
