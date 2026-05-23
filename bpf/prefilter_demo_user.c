// SPDX-License-Identifier: GPL-2.0
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct io_graph_event {
	uint32_t action_code;
};

static volatile sig_atomic_t exiting;

static void sigint_handler(int signo)
{
	(void)signo;
	exiting = 1;
}

static int read_blob(const char *path, void **blob_out, size_t *len_out)
{
	void *blob = NULL;
	long file_len;
	FILE *fp;
	int ret = 0;

	fp = fopen(path, "rb");
	if (!fp)
		return -errno;
	if (fseek(fp, 0, SEEK_END)) {
		ret = -errno;
		goto out;
	}
	file_len = ftell(fp);
	if (file_len <= 0) {
		ret = file_len < 0 ? -errno : -EINVAL;
		goto out;
	}
	if ((unsigned long)file_len > UINT32_MAX) {
		ret = -E2BIG;
		goto out;
	}
	if (fseek(fp, 0, SEEK_SET)) {
		ret = -errno;
		goto out;
	}

	blob = malloc((size_t)file_len);
	if (!blob) {
		ret = -ENOMEM;
		goto out;
	}
	if (fread(blob, 1, (size_t)file_len, fp) != (size_t)file_len) {
		ret = ferror(fp) ? -EIO : -EINVAL;
		goto out;
	}

	*blob_out = blob;
	*len_out = (size_t)file_len;
	blob = NULL;

out:
	free(blob);
	if (fclose(fp) && !ret)
		ret = -EIO;
	return ret;
}

static int handle_event(void *ctx, void *data, size_t len)
{
	const struct io_graph_event *event = data;

	(void)ctx;
	if (len < sizeof(*event))
		return 0;

	printf("action=%u\n", event->action_code);
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s PREFILTER_DEMO.bpf.o POLICY.iog\n", argv0);
}

int main(int argc, char **argv)
{
	struct bpf_object *obj = NULL;
	struct bpf_program *prog;
	struct bpf_link *link = NULL;
	struct ring_buffer *ringbuf = NULL;
	struct bpf_map *policy_map;
	void *blob = NULL;
	size_t blob_len = 0;
	uint32_t key = 0;
	int events_fd;
	int policy_fd;
	int ret;

	if (argc != 3) {
		usage(argv[0]);
		return 2;
	}

	ret = read_blob(argv[2], &blob, &blob_len);
	if (ret) {
		fprintf(stderr, "read %s: %s\n", argv[2], strerror(-ret));
		return 1;
	}

	obj = bpf_object__open_file(argv[1], NULL);
	ret = libbpf_get_error(obj);
	if (ret) {
		obj = NULL;
		fprintf(stderr, "open %s: %s\n", argv[1], strerror(-ret));
		goto out;
	}

	policy_map = bpf_object__find_map_by_name(obj, "policy");
	if (!policy_map) {
		ret = -ENOENT;
		fprintf(stderr, "policy map missing\n");
		goto out;
	}
	ret = bpf_map__set_value_size(policy_map, blob_len);
	if (ret) {
		fprintf(stderr, "set policy value size: %s\n", strerror(-ret));
		goto out;
	}
	ret = bpf_object__load(obj);
	if (ret) {
		fprintf(stderr, "load BPF object: %s\n", strerror(-ret));
		goto out;
	}

	policy_fd = bpf_map__fd(policy_map);
	ret = bpf_map_update_elem(policy_fd, &key, blob, BPF_ANY);
	if (ret) {
		ret = -errno;
		fprintf(stderr, "load policy blob: %s\n", strerror(-ret));
		goto out;
	}

	prog = bpf_object__find_program_by_name(obj, "prefilter_demo");
	if (!prog) {
		ret = -ENOENT;
		fprintf(stderr, "prefilter program missing\n");
		goto out;
	}
	link = bpf_program__attach(prog);
	ret = libbpf_get_error(link);
	if (ret) {
		link = NULL;
		fprintf(stderr, "attach prefilter program: %s\n", strerror(-ret));
		goto out;
	}

	events_fd = bpf_object__find_map_fd_by_name(obj, "events");
	if (events_fd < 0) {
		ret = events_fd;
		fprintf(stderr, "events map missing: %s\n", strerror(-ret));
		goto out;
	}
	ringbuf = ring_buffer__new(events_fd, handle_event, NULL, NULL);
	ret = libbpf_get_error(ringbuf);
	if (ret) {
		ringbuf = NULL;
		fprintf(stderr, "create ringbuf reader: %s\n", strerror(-ret));
		goto out;
	}

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
	while (!exiting) {
		ret = ring_buffer__poll(ringbuf, 100);
		if (ret == -EINTR) {
			ret = 0;
			break;
		}
		if (ret < 0) {
			fprintf(stderr, "poll ringbuf: %s\n", strerror(-ret));
			goto out;
		}
	}
	ret = 0;

out:
	ring_buffer__free(ringbuf);
	bpf_link__destroy(link);
	bpf_object__close(obj);
	free(blob);
	return ret ? 1 : 0;
}
