#ifndef PM4_TEST_COMMON_H
#define PM4_TEST_COMMON_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pm4bridge.h"

#define WAIT_MS 20000u

static inline double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#define OK(expr) do { int rc_ = (expr); if (rc_ < 0) { \
	printf("RESULT FAIL %s:%d: %s -> %s\n", __FILE__, __LINE__, #expr, pm4_strerror(rc_)); \
	exit(1); } } while (0)

/* dispatch + wait, exits on failure; returns seconds from submit to retire */
static inline double run_sync(pm4_ctx *ctx, pm4_kernel *k, uint64_t grid, uint32_t block,
			      const void *arg, size_t arg_size, pm4_fence *fence_out)
{
	double t0 = now_s();
	pm4_fence f = pm4_dispatch(ctx, k, grid, block, arg, arg_size);

	if (f < 0) {
		printf("RESULT FAIL pm4_dispatch: %s\n", pm4_strerror((int)f));
		exit(1);
	}
	OK(pm4_wait(ctx, f, WAIT_MS));
	if (fence_out)
		*fence_out = f;
	return now_s() - t0;
}

static inline const char *kpath(const char *name)
{
	static char p[256];
	const char *dir = getenv("PM4_KERNELS");

	snprintf(p, sizeof(p), "%s/%s", dir ? dir : "kernels", name);
	return p;
}

/* GPU clock ticks for a fence, 0 when PM4_GPU_TS is not enabled */
static inline uint64_t gpu_ticks(pm4_ctx *ctx, pm4_fence f)
{
	uint64_t a = 0, b = 0;

	if (pm4_gpu_ticks(ctx, f, &a, &b) != PM4_OK)
		return 0;
	return b - a;
}
#endif
