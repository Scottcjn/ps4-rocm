/* vrambw: the copybw kernels (read / write / copy) on buffers of a chosen
 * memory kind, to compare the VRAM carve-out with GTT.
 *
 * usage: t_vrambw <MB> <src kind> <dst kind> [iters] [reps]
 *   kinds: kfd   pm4_alloc (KFD userptr, system memory)       - the README baseline
 *          kfdnp pm4_alloc NONPAGED (KFD GTT BO)
 *          kfdwc pm4_alloc NONPAGED | UNCACHED
 *          gtt   libdrm amdgpu BO, GEM_DOMAIN_GTT, dmabuf-imported into KFD
 *                (cached pages; control for the import path)
 *          gttwc same, with AMDGPU_GEM_CREATE_CPU_GTT_USWC (write-combined
 *                pages, which the GPU maps without the SNOOPED bit)
 *          vram  libdrm amdgpu BO, GEM_DOMAIN_VRAM only, dmabuf-imported
 *   dst kind "none": read test only (lets a bigger source fit in VRAM)
 * The small sum-output buffer of the read test is always "kfd".
 * Residency is checked with sysfs mem_info_vram_used / mem_info_gtt_used and
 * the DRM fdinfo of the libdrm fd, before, during and after the GPU runs. */
#include "common.h"
#include <fcntl.h>
#include <unistd.h>
#include <amdgpu.h>
#include <amdgpu_drm.h>

struct bw_args { uint64_t p0, p1; uint32_t iters, block; } __attribute__((packed));
struct fb_args { uint64_t dst; uint32_t value, iters, block; } __attribute__((packed));

enum { K_KFD, K_KFDNP, K_KFDWC, K_GTT, K_GTTWC, K_VRAM, K_NONE };
static const char *kname[] = { "kfd", "kfdnp", "kfdwc", "gtt", "gttwc", "vram", "none" };

struct mem {
	int kind;
	pm4_buf buf;
	amdgpu_bo_handle bo;
	uint32_t *cpu;      /* CPU mapping (write-combined for vram) */
	uint64_t gpu;
};

static amdgpu_device_handle dev;
static int drm_fd = -1;

static long long sysfs_ll(const char *name)
{
	char p[128], b[64] = "";
	FILE *f;

	snprintf(p, sizeof(p), "/sys/class/drm/card0/device/%s", name);
	f = fopen(p, "r");
	if (!f)
		return -1;
	if (!fgets(b, sizeof(b), f))
		b[0] = 0;
	fclose(f);
	return atoll(b);
}

static void residency(const char *when)
{
	char p[64], line[256];
	FILE *f;

	if (when[0] == ' ' && !getenv("VRAMBW_STEPS"))
		return;

	printf("[mem %s] sysfs vram_used %.2f MiB  gtt_used %.2f MiB\n", when,
	       sysfs_ll("mem_info_vram_used") / 1048576.0, sysfs_ll("mem_info_gtt_used") / 1048576.0);
	if (drm_fd < 0)
		return;
	snprintf(p, sizeof(p), "/proc/self/fdinfo/%d", drm_fd);
	f = fopen(p, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "drm-memory-vram", 15) || !strncmp(line, "drm-memory-gtt", 14) ||
		    !strncmp(line, "amd-evicted-vram", 16))
			printf("    fdinfo %s", line);
	fclose(f);
}

static int parse_kind(const char *s)
{
	for (int i = 0; i < 7; i++)
		if (!strcmp(s, kname[i]))
			return i;
	printf("RESULT FAIL unknown kind %s\n", s);
	exit(2);
}

static void mem_get(pm4_ctx *ctx, struct mem *m, int kind, size_t bytes)
{
	memset(m, 0, sizeof(*m));
	m->kind = kind;
	if (kind == K_KFD || kind == K_KFDNP || kind == K_KFDWC) {
		OK(pm4_alloc(ctx, bytes, kind == K_KFDNP ? PM4_ALLOC_NONPAGED :
			     kind == K_KFDWC ? PM4_ALLOC_NONPAGED | PM4_ALLOC_UNCACHED : 0, &m->buf));
		m->cpu = m->buf.cpu;
		m->gpu = m->buf.gpu;
		return;
	}
	struct amdgpu_bo_alloc_request rq = { 0 };
	struct amdgpu_bo_info bi;
	uint32_t fd32 = 0;
	void *cpu = NULL;
	int r;

	rq.alloc_size = bytes;
	rq.phys_alignment = 4096;
	rq.preferred_heap = kind == K_VRAM ? AMDGPU_GEM_DOMAIN_VRAM : AMDGPU_GEM_DOMAIN_GTT;
	rq.flags = kind == K_VRAM ? AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED :
		   kind == K_GTTWC ? AMDGPU_GEM_CREATE_CPU_GTT_USWC : 0;
	if (getenv("VRAMBW_BO_FLAGS"))
		rq.flags = strtoull(getenv("VRAMBW_BO_FLAGS"), NULL, 0);
	r = amdgpu_bo_alloc(dev, &rq, &m->bo);
	if (r) {
		printf("RESULT FAIL amdgpu_bo_alloc(%s, %zu) = %d\n", kname[kind], bytes, r);
		exit(1);
	}
	if (!amdgpu_bo_query_info(m->bo, &bi))
		printf("  bo %s %zu MB: preferred_heap %#x (VRAM=0x4 GTT=0x2) flags %#llx size %llu\n",
		       kname[kind], bytes >> 20, bi.preferred_heap,
		       (unsigned long long)bi.alloc_flags, (unsigned long long)bi.alloc_size);
	residency("  step: after amdgpu_bo_alloc");
	r = amdgpu_bo_cpu_map(m->bo, &cpu);
	if (r) {
		printf("RESULT FAIL amdgpu_bo_cpu_map = %d\n", r);
		exit(1);
	}
	r = amdgpu_bo_export(m->bo, amdgpu_bo_handle_type_dma_buf_fd, &fd32);
	if (r) {
		printf("RESULT FAIL amdgpu_bo_export = %d\n", r);
		exit(1);
	}
	if (cpu) {
		memset(cpu, 0x5a, bytes);
		residency("  step: after cpu_map + CPU write + export");
	}
	OK(pm4_import_dmabuf(ctx, (int)fd32, bytes, &m->buf));
	residency("  step: after KFD import + map");
	close((int)fd32);
	m->cpu = cpu;
	m->gpu = m->buf.gpu;
}

static void mem_put(pm4_ctx *ctx, struct mem *m)
{
	if (m->kind == K_NONE)
		return;
	if (m->kind == K_KFD || m->kind == K_KFDNP || m->kind == K_KFDWC) {
		OK(pm4_free(ctx, &m->buf));
		return;
	}
	OK(pm4_release_import(ctx, &m->buf));
	amdgpu_bo_cpu_unmap(m->bo);
	amdgpu_bo_free(m->bo);
}

struct stat3 { double sum, best; int n; double tick_sum; };
static void st_add(struct stat3 *s, double t, uint64_t ticks)
{
	if (!s->n || t < s->best)
		s->best = t;
	s->sum += t; s->tick_sum += (double)ticks; s->n++;
}
static void st_print(const char *what, const char *sk, const char *dk, size_t bytes, uint32_t iters,
		     struct stat3 *s, uint32_t bad)
{
	double mean = s->sum / s->n, gt = s->tick_sum / s->n * 1e-8; /* 100 MHz counter */

	printf("SUMMARY %-5s src=%-5s dst=%-5s %3zu MB iters %3u: mean %6.2f GB/s  best %6.2f GB/s  gpu-timed %6.2f GB/s  (n=%d, mismatches %u)\n",
	       what, sk, dk, bytes >> 20, iters, bytes / mean / 1e9, bytes / s->best / 1e9,
	       gt > 0 ? bytes / gt / 1e9 : 0.0, s->n, bad);
}

int main(int argc, char **argv)
{
	const uint32_t block = 256;
	size_t bytes;
	uint32_t ndw, iters, i, bad, maj, min;
	int sk, dk, reps, rep, pass = 1;
	pm4_ctx *ctx;
	pm4_kernel *kcopy, *ksum, *kfill;
	struct mem S, D, O;
	uint32_t *ref, *chk;

	if (argc < 4) {
		printf("usage: t_vrambw <MB> <src kind> <dst kind|none> [iters] [reps]\n");
		return 2;
	}
	bytes = (size_t)strtoul(argv[1], NULL, 0) << 20;
	sk = parse_kind(argv[2]);
	dk = parse_kind(argv[3]);
	iters = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 64;
	reps = argc > 5 ? atoi(argv[5]) : 5;
	ndw = (uint32_t)(bytes / 4);
	uint64_t per_group = (uint64_t)block * iters;
	uint64_t groups = ndw / per_group;
	if (sk == K_NONE || !bytes || !iters || ndw % per_group) {
		printf("RESULT FAIL bad size/iters\n");
		return 2;
	}

	drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0 || amdgpu_device_initialize(drm_fd, &maj, &min, &dev)) {
		printf("RESULT FAIL amdgpu_device_initialize\n");
		return 1;
	}
	residency("start");
	OK(pm4_init(&ctx));
	OK(pm4_load(ctx, kpath("copybw.co"), "copybw", &kcopy));
	OK(pm4_load(ctx, kpath("copybw.co"), "sumbw", &ksum));
	OK(pm4_load(ctx, kpath("copybw.co"), "fillbw", &kfill));
	residency("after pm4_init");
	mem_get(ctx, &S, sk, bytes);
	if (dk != K_NONE)
		mem_get(ctx, &D, dk, bytes);
	else
		memset(&D, 0, sizeof(D)), D.kind = K_NONE;
	mem_get(ctx, &O, K_KFD, (size_t)groups * block * 4);
	printf("src %s gpu %#llx  dst %s gpu %#llx\n", kname[sk], (unsigned long long)S.gpu,
	       kname[dk], (unsigned long long)D.gpu);
	residency("after alloc+import+map");

	ref = malloc(bytes); chk = malloc(bytes);
	if (!ref || !chk)
		return 1;
	for (i = 0; i < ndw; i++)
		ref[i] = i * 2654435761u + 12345u;
	memcpy(S.cpu, ref, bytes);

	struct bw_args args = { S.gpu, D.gpu, iters, block };
	struct stat3 st;
	pm4_fence f;
	double t;

	/* read only */
	args.p1 = O.gpu;
	memset(&st, 0, sizeof(st));
	for (rep = 0; rep < reps; rep++) {
		memset(O.cpu, 0, (size_t)groups * block * 4);
		t = run_sync(ctx, ksum, groups * block, block, &args, sizeof(args), &f);
		st_add(&st, t, gpu_ticks(ctx, f));
		printf("read  rep %d: %7.3f ms %6.2f GB/s gpu_ticks %llu\n", rep, t * 1e3, bytes / t / 1e9,
		       (unsigned long long)gpu_ticks(ctx, f));
	}
	bad = 0;
	for (uint64_t g = 0; g < groups; g++)
		for (uint32_t l = 0; l < block; l++) {
			uint64_t idx = g * per_group + l;
			uint32_t sum = 0, k;

			for (k = 0; k < iters; k++, idx += block)
				sum += ref[idx];
			if (O.cpu[g * block + l] != sum && bad++ < 3)
				printf("  sum [%llu,%u] wrong\n", (unsigned long long)g, l);
		}
	pass &= !bad;
	st_print("read", kname[sk], "-", bytes, iters, &st, bad);

	if (dk != K_NONE) {
		struct fb_args fa = { D.gpu, 0, iters, block };

		/* copy */
		args.p1 = D.gpu;
		memset(&st, 0, sizeof(st));
		bad = 0;
		for (rep = 0; rep < reps; rep++) {
			memset(D.cpu, 0, bytes);
			t = run_sync(ctx, kcopy, groups * block, block, &args, sizeof(args), &f);
			st_add(&st, t, gpu_ticks(ctx, f));
			printf("copy  rep %d: %7.3f ms %6.2f GB/s copied gpu_ticks %llu\n", rep, t * 1e3,
			       bytes / t / 1e9, (unsigned long long)gpu_ticks(ctx, f));
			memcpy(chk, D.cpu, bytes);
			for (i = 0; i < ndw; i++)
				if (chk[i] != ref[i] && bad++ < 3)
					printf("  copy [%u] = %#x, want %#x\n", i, chk[i], ref[i]);
		}
		pass &= !bad;
		st_print("copy", kname[sk], kname[dk], bytes, iters, &st, bad);

		/* write only */
		memset(&st, 0, sizeof(st));
		for (rep = 0; rep < reps; rep++) {
			fa.value = 0x1000u * (uint32_t)(rep + 1);
			t = run_sync(ctx, kfill, groups * block, block, &fa, sizeof(fa), &f);
			st_add(&st, t, gpu_ticks(ctx, f));
			printf("write rep %d: %7.3f ms %6.2f GB/s gpu_ticks %llu\n", rep, t * 1e3,
			       bytes / t / 1e9, (unsigned long long)gpu_ticks(ctx, f));
		}
		bad = 0;
		memcpy(chk, D.cpu, bytes);
		for (i = 0; i < ndw; i++)
			if (chk[i] != fa.value + i && bad++ < 3)
				printf("  fill [%u] = %#x, want %#x\n", i, chk[i], fa.value + i);
		pass &= !bad;
		st_print("write", "-", kname[dk], bytes, iters, &st, bad);
	}
	/* source must be intact (and still where it was) after all GPU work */
	memcpy(chk, S.cpu, bytes);
	bad = memcmp(chk, ref, bytes) != 0;
	pass &= !bad;
	printf("source intact after runs: %s\n", bad ? "NO" : "yes");
	residency("after GPU runs");

	mem_put(ctx, &O); mem_put(ctx, &D); mem_put(ctx, &S);
	residency("after free");
	OK(pm4_unload(ctx, kcopy)); OK(pm4_unload(ctx, ksum)); OK(pm4_unload(ctx, kfill));
	OK(pm4_shutdown(ctx));
	amdgpu_device_deinitialize(dev);
	printf("RESULT %s vrambw %s->%s %zu MB\n", pass ? "PASS" : "FAIL", kname[sk], kname[dk], bytes >> 20);
	return !pass;
}
