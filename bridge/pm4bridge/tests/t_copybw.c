/* copybw: copy a 256 MB buffer to another on the GPU and report GB/s.
 * Also read-only (sumbw) and write-only (fillbw) passes.
 * usage: t_copybw [iters ...]   (dwords copied per work item, default 16 64 256) */
#include "common.h"

struct bw_args { uint64_t p0, p1; uint32_t iters, block; } __attribute__((packed));
struct fb_args { uint64_t dst; uint32_t value, iters, block; } __attribute__((packed));

#define BYTES (256u << 20)
#define NDW   (BYTES / 4u)

int main(int argc, char **argv)
{
	uint32_t iters_list[8] = { 16, 64, 256 };
	int n_iters = 3, pass = 1, a, rep;
	const uint32_t block = 256;
	pm4_ctx *ctx;
	pm4_kernel *kcopy, *ksum, *kfill;
	pm4_buf S, D, O;
	uint32_t *s, *d, *o, i;
	double cpu_t0, cpu_t;

	if (argc > 1) {
		n_iters = 0;
		for (a = 1; a < argc && n_iters < 8; a++)
			iters_list[n_iters++] = (uint32_t)strtoul(argv[a], NULL, 0);
	}
	OK(pm4_init(&ctx));
	OK(pm4_load(ctx, kpath("copybw.co"), "copybw", &kcopy));
	OK(pm4_load(ctx, kpath("copybw.co"), "sumbw", &ksum));
	OK(pm4_load(ctx, kpath("copybw.co"), "fillbw", &kfill));
	OK(pm4_alloc(ctx, BYTES, 0, &S));
	OK(pm4_alloc(ctx, BYTES, 0, &D));
	OK(pm4_alloc(ctx, BYTES / 16, 0, &O));
	s = S.cpu; d = D.cpu; o = O.cpu;
	for (i = 0; i < NDW; i++)
		s[i] = i * 2654435761u + 12345u;

	/* CPU reference for the same copy */
	cpu_t0 = now_s();
	memcpy(d, s, BYTES);
	cpu_t = now_s() - cpu_t0;
	printf("cpu memcpy 256 MB: %.1f ms = %.2f GB/s copied (read+write traffic %.2f GB/s)\n",
	       cpu_t * 1e3, BYTES / cpu_t / 1e9, 2.0 * BYTES / cpu_t / 1e9);

	for (a = 0; a < n_iters; a++) {
		uint32_t iters = iters_list[a];
		uint64_t per_group = (uint64_t)block * iters;
		uint64_t groups = NDW / per_group;
		struct bw_args args = { S.gpu, D.gpu, iters, block };
		struct fb_args fa = { D.gpu, 0, iters, block };
		pm4_fence f;
		uint64_t ticks;
		uint32_t bad;
		double t, best;

		if (!iters || NDW % per_group) {
			printf("iters %u: 256 MB is not a multiple of block*iters, skipped\n", iters);
			continue;
		}
		/* copy */
		best = 1e9;
		for (rep = 0; rep < 3; rep++) {
			memset(d, 0, BYTES);
			t = run_sync(ctx, kcopy, groups * block, block, &args, sizeof(args), &f);
			ticks = gpu_ticks(ctx, f);
			if (t < best)
				best = t;
			bad = 0;
			for (i = 0; i < NDW; i++)
				if (d[i] != s[i] && bad++ < 3)
					printf("  copy [%u] = %#x, want %#x\n", i, d[i], s[i]);
			pass &= !bad;
			printf("copy  iters %4u rep %d: %7.2f ms  %6.2f GB/s copied (%.2f GB/s traffic) gpu_ticks %llu  mismatches %u\n",
			       iters, rep, t * 1e3, BYTES / t / 1e9, 2.0 * BYTES / t / 1e9,
			       (unsigned long long)ticks, bad);
		}
		printf("copybw iters %u: best %.2f GB/s copied = %.2f GB/s read+write traffic\n",
		       iters, BYTES / best / 1e9, 2.0 * BYTES / best / 1e9);

		/* read only */
		args.p1 = O.gpu;
		best = 1e9;
		for (rep = 0; rep < 3; rep++) {
			memset(o, 0, (size_t)groups * block * 4);
			t = run_sync(ctx, ksum, groups * block, block, &args, sizeof(args), &f);
			ticks = gpu_ticks(ctx, f);
			if (t < best)
				best = t;
			printf("read  iters %4u rep %d: %7.2f ms  %6.2f GB/s  gpu_ticks %llu\n",
			       iters, rep, t * 1e3, BYTES / t / 1e9, (unsigned long long)ticks);
		}
		bad = 0;
		for (uint64_t g = 0; g < groups; g++)
			for (uint32_t l = 0; l < block; l++) {
				uint64_t idx = g * per_group + l;
				uint32_t sum = 0, k;

				for (k = 0; k < iters; k++, idx += block)
					sum += s[idx];
				if (o[g * block + l] != sum && bad++ < 3)
					printf("  sum [%llu,%u] = %#x, want %#x\n",
					       (unsigned long long)g, l, o[g * block + l], sum);
			}
		pass &= !bad;
		printf("readbw iters %u: best %.2f GB/s read, mismatches %u\n", iters, BYTES / best / 1e9, bad);

		/* write only */
		best = 1e9;
		for (rep = 0; rep < 3; rep++) {
			fa.value = 0x1000u * (uint32_t)(rep + 1);
			t = run_sync(ctx, kfill, groups * block, block, &fa, sizeof(fa), &f);
			ticks = gpu_ticks(ctx, f);
			if (t < best)
				best = t;
			printf("write iters %4u rep %d: %7.2f ms  %6.2f GB/s  gpu_ticks %llu\n",
			       iters, rep, t * 1e3, BYTES / t / 1e9, (unsigned long long)ticks);
		}
		bad = 0;
		for (i = 0; i < NDW; i++)
			if (d[i] != fa.value + i && bad++ < 3)
				printf("  fill [%u] = %#x, want %#x\n", i, d[i], fa.value + i);
		pass &= !bad;
		printf("writebw iters %u: best %.2f GB/s written, mismatches %u\n", iters, BYTES / best / 1e9, bad);
	}

	OK(pm4_free(ctx, &S)); OK(pm4_free(ctx, &D)); OK(pm4_free(ctx, &O));
	OK(pm4_unload(ctx, kcopy)); OK(pm4_unload(ctx, ksum)); OK(pm4_unload(ctx, kfill));
	OK(pm4_shutdown(ctx));
	printf("RESULT %s copybw\n", pass ? "PASS" : "FAIL");
	return !pass;
}
