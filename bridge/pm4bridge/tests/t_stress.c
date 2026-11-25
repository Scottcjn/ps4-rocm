/* stress: 1000 back-to-back small dispatches. Each increments 64 counters
 * (read-modify-write across dispatches) and writes one log entry, so a lost
 * or duplicated dispatch, or a stale cache, is visible.
 * Phase 1: synchronous (dispatch + wait each) -> round-trip latency.
 * Phase 2: asynchronous (submit all, wait for the last) -> throughput. */
#include "common.h"

struct st_args { uint64_t counter, log; uint32_t idx, block; } __attribute__((packed));

#define N_DISP 1000u
#define ITEMS  64u

static int verify(const uint32_t *cnt, const uint32_t *log, uint32_t expect, const char *what)
{
	uint32_t i, bad = 0;

	for (i = 0; i < ITEMS; i++)
		if (cnt[i] != expect && bad++ < 5)
			printf("  %s: counter[%u] = %u, want %u\n", what, i, cnt[i], expect);
	for (i = 0; i < N_DISP; i++)
		if (log[i] != i + 1 && bad++ < 5)
			printf("  %s: log[%u] = %u, want %u (dispatch lost)\n", what, i, log[i], i + 1);
	printf("%s: %u errors\n", what, bad);
	return bad == 0;
}

int main(void)
{
	pm4_ctx *ctx;
	pm4_kernel *k;
	pm4_buf C, L;
	struct st_args a;
	uint32_t *cnt, *log, i;
	double t0, t, tmin = 1e9, tmax = 0;
	pm4_fence f = 0;
	int pass = 1;

	OK(pm4_init(&ctx));
	OK(pm4_load(ctx, kpath("stress.co"), "stress", &k));
	OK(pm4_alloc(ctx, 4096, PM4_ALLOC_ZERO, &C));
	OK(pm4_alloc(ctx, N_DISP * 4, PM4_ALLOC_ZERO, &L));
	cnt = C.cpu; log = L.cpu;
	a.counter = C.gpu; a.log = L.gpu; a.block = ITEMS;

	t0 = now_s();
	for (i = 0; i < N_DISP; i++) {
		a.idx = i;
		t = run_sync(ctx, k, ITEMS, ITEMS, &a, sizeof(a), NULL);
		if (t < tmin) tmin = t;
		if (t > tmax) tmax = t;
	}
	t = now_s() - t0;
	printf("sync : %u dispatches in %.2f ms = %.1f us/dispatch (min %.1f, max %.1f)\n",
	       N_DISP, t * 1e3, t * 1e6 / N_DISP, tmin * 1e6, tmax * 1e6);
	pass &= verify(cnt, log, N_DISP, "sync");

	memset(log, 0, N_DISP * 4);
	t0 = now_s();
	for (i = 0; i < N_DISP; i++) {
		a.idx = i;
		f = pm4_dispatch(ctx, k, ITEMS, ITEMS, &a, sizeof(a));
		if (f < 0) {
			printf("RESULT FAIL pm4_dispatch %u: %s\n", i, pm4_strerror((int)f));
			return 1;
		}
	}
	OK(pm4_wait(ctx, f, WAIT_MS));
	t = now_s() - t0;
	printf("async: %u dispatches in %.2f ms = %.1f us/dispatch\n", N_DISP, t * 1e3, t * 1e6 / N_DISP);
	pass &= verify(cnt, log, 2 * N_DISP, "async");

	OK(pm4_free(ctx, &C)); OK(pm4_free(ctx, &L));
	OK(pm4_unload(ctx, k));
	OK(pm4_shutdown(ctx));
	printf("RESULT %s stress %u+%u dispatches\n", pass ? "PASS" : "FAIL", N_DISP, N_DISP);
	return !pass;
}
