/* noop: the smallest possible dispatch - proves queue, IB, fence and loader */
#include "common.h"

int main(void)
{
	pm4_ctx *ctx;
	pm4_kernel *k;
	int i;

	OK(pm4_init(&ctx));
	OK(pm4_load(ctx, kpath("noop.co"), "noop", &k));
	for (i = 0; i < 3; i++) {
		double t = run_sync(ctx, k, 64, 64, NULL, 0, NULL);
		printf("noop dispatch %d retired in %.1f us\n", i, t * 1e6);
	}
	OK(pm4_unload(ctx, k));
	OK(pm4_shutdown(ctx));
	printf("RESULT PASS noop\n");
	return 0;
}
