/* vadd: c = a + b over 16M floats, verified on the CPU */
#include "common.h"

struct vadd_args { uint64_t a, b, c; uint32_t n, block; } __attribute__((packed));

int main(void)
{
	const uint32_t n = 16u << 20, block = 256;
	pm4_ctx *ctx;
	pm4_kernel *k;
	pm4_buf A, B, C;
	struct vadd_args args;
	float *a, *b, *c;
	uint32_t i, bad = 0;
	double t;
	int rep;

	OK(pm4_init(&ctx));
	OK(pm4_load(ctx, kpath("vadd.co"), "vadd", &k));
	OK(pm4_alloc(ctx, (size_t)n * 4, 0, &A));
	OK(pm4_alloc(ctx, (size_t)n * 4, 0, &B));
	OK(pm4_alloc(ctx, (size_t)n * 4, 0, &C));
	a = A.cpu; b = B.cpu; c = C.cpu;
	args.a = A.gpu; args.b = B.gpu; args.c = C.gpu; args.n = n; args.block = block;

	for (rep = 0; rep < 2; rep++) {
		for (i = 0; i < n; i++) {
			a[i] = (float)(i % 4099) * 0.25f + (float)rep;
			b[i] = (float)(i % 8191) * -0.5f;
			c[i] = -12345.0f;
		}
		t = run_sync(ctx, k, n, block, &args, sizeof(args), NULL);
		for (i = 0; i < n; i++)
			if (c[i] != a[i] + b[i] && bad++ < 5)
				printf("  rep %d [%u] = %g, want %g\n", rep, i, c[i], a[i] + b[i]);
		printf("vadd rep %d: %u floats in %.2f ms (%.2f GB/s moved, 12 B/elem), %u mismatches\n",
		       rep, n, t * 1e3, (double)n * 12 / t / 1e9, bad);
	}
	OK(pm4_free(ctx, &A)); OK(pm4_free(ctx, &B)); OK(pm4_free(ctx, &C));
	OK(pm4_unload(ctx, k));
	OK(pm4_shutdown(ctx));
	printf("RESULT %s vadd n=%u\n", bad ? "FAIL" : "PASS", n);
	return bad != 0;
}
