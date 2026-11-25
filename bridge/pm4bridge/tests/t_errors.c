/* errors: API misuse and unusable code objects must fail cleanly.
 * Nothing in here dispatches a kernel that could hang the GPU. */
#include "common.h"

static int fails;

#define EXPECT(expr, want) do { long long got_ = (long long)(expr); \
	printf("%-58s -> %-36s %s\n", #expr, pm4_strerror((int)got_), got_ == (want) ? "ok" : "UNEXPECTED"); \
	if (got_ != (want)) fails++; } while (0)

int main(void)
{
	pm4_ctx *ctx;
	pm4_kernel *k = NULL, *noop;
	pm4_buf b;
	uint32_t arg[5] = { 0 };

	OK(pm4_init(&ctx));
	EXPECT(pm4_load(ctx, kpath("does-not-exist.co"), "fill", &k), PM4_ERR_IO);
	EXPECT(pm4_load(ctx, "Makefile", "fill", &k), PM4_ERR_ELF);
	EXPECT(pm4_load(ctx, kpath("fill.co"), "no_such_kernel", &k), PM4_ERR_NOSYM);
	EXPECT(pm4_load(ctx, kpath("bad/scratch.co"), "scratch", &k), PM4_ERR_UNSUP);
	EXPECT(pm4_alloc(ctx, 0, 0, &b), PM4_ERR_ARG);
	OK(pm4_load(ctx, kpath("fill.co"), "fill", &k));
	OK(pm4_load(ctx, kpath("noop.co"), "noop", &noop));
	EXPECT(pm4_dispatch(ctx, k, 0, 64, arg, sizeof(arg)), PM4_ERR_ARG);
	EXPECT(pm4_dispatch(ctx, k, 64, 0, arg, sizeof(arg)), PM4_ERR_ARG);
	EXPECT(pm4_dispatch(ctx, k, 1024, 512, arg, sizeof(arg)), PM4_ERR_ARG);
	EXPECT(pm4_dispatch(ctx, k, 64, 64, arg, 8), PM4_ERR_ARG);      /* kernarg too small */
	EXPECT(pm4_dispatch(ctx, k, 64, 64, NULL, 20), PM4_ERR_ARG);
	EXPECT(pm4_wait(ctx, 1, 100), PM4_ERR_ARG);                     /* nothing dispatched yet */
	EXPECT(pm4_wait(ctx, 0, 100), PM4_ERR_ARG);
	EXPECT(pm4_dispatch(ctx, noop, 64, 64, NULL, 0), 1);            /* first fence id is 1 */
	EXPECT(pm4_wait(ctx, 1, 5000), PM4_OK);
	EXPECT(pm4_wait(ctx, 1, 0), PM4_OK);                            /* already retired */
	EXPECT(pm4_wait(ctx, 2, 100), PM4_ERR_ARG);                     /* not issued */
	OK(pm4_unload(ctx, noop));
	OK(pm4_unload(ctx, k));
	OK(pm4_shutdown(ctx));
	printf("RESULT %s errors (%d unexpected)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
