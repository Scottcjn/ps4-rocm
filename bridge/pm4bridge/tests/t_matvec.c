/* matvec: f32 y = A x, 4096 x 4096, verified against the CPU.
 * Two layouts: row-major (matvec) and column-major (matvec_t).
 * usage: t_matvec [block]   (work-group size, default 64) */
#include <math.h>
#include "common.h"

struct mv_args { uint64_t A, x, y; uint32_t n, block; } __attribute__((packed));

#define N 4096u

static int check(const float *y, const float *ref, const char *what)
{
	uint32_t i, bad = 0;
	double maxrel = 0;

	for (i = 0; i < N; i++) {
		double e = fabs((double)y[i] - (double)ref[i]);
		double rel = e / (fabs((double)ref[i]) + 1e-6);

		if (rel > maxrel)
			maxrel = rel;
		if (!(rel <= 1e-4) && bad++ < 5)
			printf("  %s: y[%u] = %g, want %g\n", what, i, y[i], ref[i]);
	}
	printf("%s: max rel err %.3g, %u rows out of tolerance\n", what, maxrel, bad);
	return bad == 0;
}

int main(int argc, char **argv)
{
	uint32_t block = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 64;
	pm4_ctx *ctx;
	pm4_kernel *k, *kt;
	pm4_buf A, At, X, Y;
	struct mv_args args;
	float *a, *at, *x, *y, *ref;
	uint32_t i, j, seed = 12345;
	double t, best, cpu_t;
	int pass = 1, rep;
	const double flops = 2.0 * N * N, bytes = 4.0 * N * N;

	OK(pm4_init(&ctx));
	OK(pm4_load(ctx, kpath("matvec.co"), "matvec", &k));
	OK(pm4_load(ctx, kpath("matvec.co"), "matvec_t", &kt));
	OK(pm4_alloc(ctx, (size_t)N * N * 4, 0, &A));
	OK(pm4_alloc(ctx, (size_t)N * N * 4, 0, &At));
	OK(pm4_alloc(ctx, N * 4, 0, &X));
	OK(pm4_alloc(ctx, N * 4, 0, &Y));
	a = A.cpu; at = At.cpu; x = X.cpu; y = Y.cpu;
	ref = malloc(N * sizeof(float));
	if (!ref)
		return 1;
	for (i = 0; i < N; i++) {
		for (j = 0; j < N; j++) {
			seed = seed * 1664525u + 1013904223u;
			a[(size_t)i * N + j] = (float)(seed >> 8) / 16777216.0f - 0.5f;
		}
		seed = seed * 1664525u + 1013904223u;
		x[i] = (float)(seed >> 8) / 16777216.0f - 0.5f;
	}
	for (i = 0; i < N; i++)
		for (j = 0; j < N; j++)
			at[(size_t)j * N + i] = a[(size_t)i * N + j];

	/* CPU reference, same summation order as the kernel (single thread) */
	t = now_s();
	for (i = 0; i < N; i++) {
		float s = 0.0f;

		for (j = 0; j < N; j++)
			s += a[(size_t)i * N + j] * x[j];
		ref[i] = s;
	}
	cpu_t = now_s() - t;
	printf("cpu 1 thread: %.2f ms, %.2f GFLOP/s, %.2f GB/s\n", cpu_t * 1e3, flops / cpu_t / 1e9, bytes / cpu_t / 1e9);

	args.x = X.gpu; args.y = Y.gpu; args.n = N; args.block = block;

	args.A = A.gpu;
	best = 1e9;
	for (rep = 0; rep < 3; rep++) {
		pm4_fence f;

		memset(y, 0, N * 4);
		t = run_sync(ctx, k, N, block, &args, sizeof(args), &f);
		if (t < best)
			best = t;
		printf("matvec   block %u rep %d: %.2f ms gpu_ticks %llu\n", block, rep, t * 1e3,
		       (unsigned long long)gpu_ticks(ctx, f));
		pass &= check(y, ref, "matvec (row-major)");
	}
	printf("matvec row-major: best %.2f ms = %.2f GFLOP/s, %.2f GB/s of matrix\n",
	       best * 1e3, flops / best / 1e9, bytes / best / 1e9);

	args.A = At.gpu;
	best = 1e9;
	for (rep = 0; rep < 3; rep++) {
		pm4_fence f;

		memset(y, 0, N * 4);
		t = run_sync(ctx, kt, N, block, &args, sizeof(args), &f);
		if (t < best)
			best = t;
		printf("matvec_t block %u rep %d: %.2f ms gpu_ticks %llu\n", block, rep, t * 1e3,
		       (unsigned long long)gpu_ticks(ctx, f));
		pass &= check(y, ref, "matvec_t (column-major)");
	}
	printf("matvec column-major: best %.2f ms = %.2f GFLOP/s, %.2f GB/s of matrix\n",
	       best * 1e3, flops / best / 1e9, bytes / best / 1e9);

	free(ref);
	OK(pm4_free(ctx, &A)); OK(pm4_free(ctx, &At)); OK(pm4_free(ctx, &X)); OK(pm4_free(ctx, &Y));
	OK(pm4_unload(ctx, k)); OK(pm4_unload(ctx, kt));
	OK(pm4_shutdown(ctx));
	printf("RESULT %s matvec n=%u block=%u\n", pass ? "PASS" : "FAIL", N, block);
	return !pass;
}
