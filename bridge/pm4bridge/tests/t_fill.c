/* fill: GPU writes value+i, CPU verifies. Done twice with different values
 * into the same buffer (stale L1/L2 would show the first value), then the CPU
 * rewrites the buffer and the GPU does a read-modify-write (the GPU must see
 * CPU writes), twice. */
#include "common.h"

struct fill_args { uint64_t out; uint32_t value, n, block; } __attribute__((packed));

static int verify(const uint32_t *p, uint32_t n, uint32_t base, uint32_t guard_n, const char *what)
{
	uint32_t i, bad = 0;

	for (i = 0; i < n; i++)
		if (p[i] != base + i && bad++ < 5)
			printf("  %s: [%u] = %#x, want %#x\n", what, i, p[i], base + i);
	for (i = n; i < guard_n; i++)
		if (p[i] != 0xA5A5A5A5u && bad++ < 5)
			printf("  %s: guard [%u] = %#x overwritten\n", what, i, p[i]);
	printf("%s: %u mismatches of %u\n", what, bad, n);
	return bad == 0;
}

int main(int argc, char **argv)
{
	uint32_t v1 = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0x11110000u;
	uint32_t v2 = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0x77770000u;
	const uint32_t n = 1000037, block = 256;       /* not a multiple of block */
	const uint32_t guard_n = (n + block + 1023) & ~1023u;
	pm4_ctx *ctx;
	pm4_kernel *kfill, *kadd;
	pm4_buf buf;
	struct fill_args a;
	uint32_t *p, i;
	int pass = 1;

	OK(pm4_init(&ctx));
	OK(pm4_load(ctx, kpath("fill.co"), "fill", &kfill));
	OK(pm4_load(ctx, kpath("fill.co"), "addk", &kadd));
	OK(pm4_alloc(ctx, (size_t)guard_n * 4, 0, &buf));
	p = buf.cpu;
	for (i = 0; i < guard_n; i++)
		p[i] = 0xA5A5A5A5u;

	a.out = buf.gpu; a.n = n; a.block = block;
	a.value = v1;
	run_sync(ctx, kfill, n, block, &a, sizeof(a), NULL);
	pass &= verify(p, n, v1, guard_n, "fill #1");
	a.value = v2;
	run_sync(ctx, kfill, n, block, &a, sizeof(a), NULL);
	pass &= verify(p, n, v2, guard_n, "fill #2 (same buffer, new value)");

	for (i = 0; i < n; i++)
		p[i] = 0x1000u + i;                   /* CPU write the GPU must see */
	a.value = 5;
	run_sync(ctx, kadd, n, block, &a, sizeof(a), NULL);
	pass &= verify(p, n, 0x1005u, guard_n, "addk #1 (after CPU write)");
	for (i = 0; i < n; i++)
		p[i] = 0x200000u + i;
	a.value = 9;
	run_sync(ctx, kadd, n, block, &a, sizeof(a), NULL);
	pass &= verify(p, n, 0x200009u, guard_n, "addk #2 (after CPU rewrite)");
	a.value = 1;
	run_sync(ctx, kadd, n, block, &a, sizeof(a), NULL);
	pass &= verify(p, n, 0x20000Au, guard_n, "addk #3 (GPU on GPU result)");

	OK(pm4_free(ctx, &buf));
	OK(pm4_unload(ctx, kadd));
	OK(pm4_unload(ctx, kfill));
	OK(pm4_shutdown(ctx));
	printf("RESULT %s fill v1=%#x v2=%#x\n", pass ? "PASS" : "FAIL", v1, v2);
	return !pass;
}
