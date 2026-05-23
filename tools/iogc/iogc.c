#include <iog/compile.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_blob(const char *path, const void *blob, size_t len)
{
	FILE *fp = fopen(path, "wb");

	if (!fp)
		return -errno;
	if (fwrite(blob, 1, len, fp) != len) {
		int ret = ferror(fp) ? -EIO : -ENOSPC;

		fclose(fp);
		return ret;
	}
	if (fclose(fp))
		return -EIO;
	return 0;
}

static int write_meta(const char *path, const struct iog_compile_stats *stats,
		      u32 max_input_len)
{
	FILE *fp = fopen(path, "w");

	if (!fp)
		return -errno;
	if (fprintf(fp,
		    "{\n"
		    "  \"max_input_len\": %u,\n"
		    "  \"max_prefix_len\": %u,\n"
		    "  \"max_probe_len\": %u,\n"
		    "  \"node_cnt\": %u,\n"
		    "  \"edge_cnt\": %u,\n"
		    "  \"accept_cnt\": %u,\n"
		    "  \"blob_bytes\": %llu\n"
		    "}\n",
		    max_input_len, stats->max_prefix_len,
		    stats->max_probe_len, stats->node_cnt, stats->edge_cnt,
		    stats->accept_cnt,
		    (unsigned long long)stats->blob_bytes) < 0) {
		fclose(fp);
		return -EIO;
	}
	if (fclose(fp))
		return -EIO;
	return 0;
}

int main(int argc, char **argv)
{
	struct iog_prefix *prefixes;
	struct iog_compile_stats stats;
	char **lines;
	char err[256] = "";
	char buf[4096];
	void *blob = NULL;
	size_t blob_len = 0;
	size_t nr = 0, cap = 0;
	FILE *fp;
	int ret = 0;

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: %s PREFIXES.txt POLICY.iog [POLICY.meta.json]\n",
			argv[0]);
		return 2;
	}

	fp = fopen(argv[1], "r");
	if (!fp) {
		perror(argv[1]);
		return 1;
	}

	prefixes = NULL;
	lines = NULL;
	while (fgets(buf, sizeof(buf), fp)) {
		size_t len = strcspn(buf, "\r\n");
		char *line;

		if (!len || buf[0] == '#')
			continue;
		if (nr == cap) {
			size_t next = cap ? cap * 2 : 64;
			void *p = realloc(prefixes, next * sizeof(*prefixes));
			void *l = realloc(lines, next * sizeof(*lines));

			if (!p || !l) {
				free(p);
				free(l);
				ret = -ENOMEM;
				goto out;
			}
			prefixes = p;
			lines = l;
			cap = next;
		}
		line = malloc(len + 1);
		if (!line) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(line, buf, len);
		line[len] = '\0';
		lines[nr] = line;
		prefixes[nr].bytes = (const u8 *)line;
		prefixes[nr].len = (u32)len;
		prefixes[nr].action_code = 1;
		nr++;
	}
	if (ferror(fp)) {
		ret = -EIO;
		goto out;
	}
	if (!nr) {
		ret = -EINVAL;
		goto out;
	}

	ret = iog_compile_prefixes(prefixes, nr, 4096, &blob, &blob_len,
				   &stats, err, sizeof(err));
	if (ret)
		goto out;
	ret = write_blob(argv[2], blob, blob_len);
	if (ret)
		goto out;
	if (argc == 4)
		ret = write_meta(argv[3], &stats, 4096);

out:
	if (ret) {
		if (err[0])
			fprintf(stderr, "iogc: %s: %s\n", err, strerror(-ret));
		else
			fprintf(stderr, "iogc: %s\n", strerror(-ret));
	}
	free(blob);
	while (nr)
		free(lines[--nr]);
	free(lines);
	free(prefixes);
	fclose(fp);
	return ret ? 1 : 0;
}
