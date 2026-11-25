/* wide (experimental): 16-byte buffer_load/store_dwordx4 accesses.
 * Separates a bytes/s memory limit from a requests/s limit.
 * usage: t_wide [megabytes] [iters]   (default 256 MB, 64 uint4 per item) */
#include "common.h"

struct bw_args { uint64_t p0, p1; uint32_t iters, block; } __attribute__((packed));

int main(int argc, char **argv)
{
	size_t bytes = (size_t)(argc > 1 ? strtoul(argv[1], NULL, 0) : 256) << 20;
	uint32_t iters = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 64;
	const uint32_t block = 256;
	size_t nv = bytes / 16, ndw = bytes / 4;
	uint64_t per_group = (uint64_t)block * iters, groups = nv / per_group;
	pm4_ctx *ctx;
	pm4_kernel *ksum, *kcopy;
	pm4_buf S, D, O;
	struct bw_args args;
	uint32_t *s, *d, *o, bad = 0;
	double t, best;
	size_t i;
	int rep, pass = 1;

	if (!iters || !groups || nv % per_group) {
		printf("RESULT FAIL wide: size is not a multiple of block*iters*16\n");
		return 1;
	}
	OK(pm4_init(&ctx));
	OK(pm4_load(ctx, kpath("wide.co"), "sumbw4", &ksum));
	OK(pm4_load(ctx, kpath("wide.co"), "copybw4", &kcopy));
	OK(pm4_alloc(ctx, bytes, 0, &S));
	OK(pm4_alloc(ctx, bytes, 0, &D));
	OK(pm4_alloc(ctx, (size_t)groups * block * 4, 0, &O));
	s = S.cpu; d = D.cpu; o = O.cpu;
	for (i = 0; i < ndw; i++)
		s[i] = (uint32_t)i * 2654435761u + 12345u;
	args.p0 = S.gpu; args.iters = iters; args.block = block;

	args.p1 = O.gpu;
	best = 1e9;
	for (rep = 0; rep < 3; rep++) {
		memset(o, 0, (size_t)groups * block * 4);
		t = run_sync(ctx, ksum, groups * block, block, &args, sizeof(args), NULL);
		if (t < best)
			best = t;
	}
	for (uint64_t g = 0; g < groups; g++)
		for (uint32_t l = 0; l < block; l++) {
			uint64_t idx = g * per_group + l;
			uint32_t sum = 0, k;

			for (k = 0; k < iters; k++, idx += block)
				sum += s[idx * 4] + s[idx * 4 + 1] + s[idx * 4 + 2] + s[idx * 4 + 3];
			if (o[g * block + l] != sum && bad++ < 3)
				printf("  sum4 [%llu,%u] = %#x, want %#x\n",
				       (unsigned long long)g, l, o[g * block + l], sum);
		}
	pass &= !bad;
	printf("readbw4  %zu MB iters %u: best %.2f ms = %.2f GB/s read, mismatches %u\n",
	       bytes >> 20, iters, best * 1e3, bytes / best / 1e9, bad);

	args.p1 = D.gpu;
	best = 1e9;
	bad = 0;
	for (rep = 0; rep < 3; rep++) {
		memset(d, 0, bytes);
		t = run_sync(ctx, kcopy, groups * block, block, &args, sizeof(args), NULL);
		if (t < best)
			best = t;
		for (i = 0; i < ndw; i++)
			if (d[i] != s[i] && bad++ < 3)
				printf("  copy4 [%zu] = %#x, want %#x\n", i, d[i], s[i]);
	}
	pass &= !bad;
	printf("copybw4  %zu MB iters %u: best %.2f ms = %.2f GB/s copied = %.2f GB/s read+write traffic, mismatches %u\n",
	       bytes >> 20, iters, best * 1e3, bytes / best / 1e9, 2.0 * bytes / best / 1e9, bad);

	OK(pm4_free(ctx, &S)); OK(pm4_free(ctx, &D)); OK(pm4_free(ctx, &O));
	OK(pm4_unload(ctx, ksum)); OK(pm4_unload(ctx, kcopy));
	OK(pm4_shutdown(ctx));
	printf("RESULT %s wide %zu MB\n", pass ? "PASS" : "FAIL", bytes >> 20);
	return !pass;
}
