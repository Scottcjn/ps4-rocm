/*
 * pm4bridge - run clang-compiled amdgcn (gfx7) kernels on the PS4 Pro GPU
 * through KFD PM4 compute queues.
 *
 * The PM4 packet encodings, the indirect-buffer layout and the ring/doorbell
 * handling are adapted from the ROCm kfdtest sources (ROCR-Runtime,
 * libhsakmt/tests/kfdtest/src: Dispatch.cpp, PM4Packet.cpp, PM4Queue.cpp,
 * BaseQueue.cpp, KFDTestUtil.cpp), which are
 *   Copyright (C) 2014-2018 Advanced Micro Devices, Inc. All Rights Reserved.
 * and distributed under the MIT license:
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to permit
 *   persons to whom the Software is furnished to do so, subject to the
 *   following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 *   NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 *   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 *   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 *   USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#define _GNU_SOURCE
#include "pm4bridge.h"

#include <elf.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hsakmt/hsakmt.h"

/* ---- sizes ------------------------------------------------------------ */
#define PM4_SLOTS        64u           /* dispatches that may be in flight */
#define IB_DWORDS        128u          /* per-slot indirect buffer */
#define IB_BYTES         (IB_DWORDS * 4u)
#define RING_BYTES       (16u * 1024u) /* must be a power of two */
#define RING_DWORDS      (RING_BYTES / 4u)
#define FENCE_BYTES      4096u
#define TS_OFFSET        64u           /* timestamps: fence page + 64 + slot*16 */
#define DEFAULT_TIMEOUT_MS 10000u      /* internal waits (slot reuse, ring space) */

/* ---- PM4 (gfx7 / CI MEC), values as used by kfdtest -------------------- */
#define IT_NOP             0x10
#define IT_DISPATCH_DIRECT 0x15
#define IT_WAIT_REG_MEM    0x3C
#define IT_INDIRECT_BUFFER 0x3F
#define IT_RELEASE_MEM     0x49
#define IT_ACQUIRE_MEM     0x58
#define IT_SET_SH_REG      0x76
#define CMD_NOP_TYPE_3     0xFFFF1002u   /* one-dword NOP used to pad the ring */

/* type-3 header: predicate:1 shaderType:1(compute) rsvd:6 opcode:8 count:14 type:2 */
#define PM4_HDR(op, ndw) ((3u << 30) | ((((ndw) - 2u) & 0x3FFFu) << 16) | ((uint32_t)(op) << 8) | 2u)

#define PERSISTENT_SPACE_START 0x2C00u
#define mmCOMPUTE_START_X                 0x2E04u
#define mmCOMPUTE_PGM_LO                  0x2E0Cu
#define mmCOMPUTE_PGM_RSRC1               0x2E12u
#define mmCOMPUTE_RESOURCE_LIMITS         0x2E15u
#define mmCOMPUTE_STATIC_THREAD_MGMT_SE0  0x2E16u
#define mmCOMPUTE_TMPRING_SIZE            0x2E18u
#define mmCOMPUTE_RESTART_X               0x2E1Bu
#define mmCOMPUTE_USER_DATA_0             0x2E40u

#define RSRC2_SCRATCH_EN       (1u << 0)
#define RSRC2_USER_SGPR(x)     (((x) >> 1) & 0x1Fu)
#define RSRC2_LDS_SIZE_SHIFT   15
#define RSRC2_LDS_SIZE_MASK    (0x1FFu << 15)

/* CP_COHER_CNTL bits (gfx7): TCL1 = 22, TC(L2) = 23, SH_KCACHE = 27, SH_ICACHE = 29 */
#define COHER_PRE   0x28C00000u  /* L1 + L2 + K$ + I$ invalidate: kfdtest's value */
#define COHER_POST  0x08400000u  /* L1 + K$ invalidate after each dispatch */

/* COMPUTE_SHADER_EN only. kfdtest uses 0x21 (| USE_THREAD_DIMENSIONS), which
 * is harmless there because it always dispatches 1 thread per group; with
 * that bit set this GPU interprets DISPATCH_DIRECT dim_x as a *thread* count,
 * not a group count (measured: 3907 "groups" of 256 wrote exactly 3907
 * elements). With the bit clear dim_x counts work groups of NUM_THREAD_X. */
#define DISPATCH_INITIATOR 0x1u

/* RELEASE_MEM selectors */
#define RM_INT_NONE_WAIT_WC   3u  /* wait for write confirm, no interrupt */
#define RM_DATA_32            1u
#define RM_DATA_GPU_CLOCK     3u

/* ---- amdhsa kernel descriptor (64 bytes) ------------------------------- */
struct kernel_descriptor {
	uint32_t group_segment_fixed_size;
	uint32_t private_segment_fixed_size;
	uint32_t kernarg_size;
	uint8_t  reserved0[4];
	int64_t  kernel_code_entry_byte_offset;
	uint8_t  reserved1[20];
	uint32_t compute_pgm_rsrc3;
	uint32_t compute_pgm_rsrc1;
	uint32_t compute_pgm_rsrc2;
	uint16_t kernel_code_properties;
	uint16_t kernarg_preload;
	uint8_t  reserved2[4];
};
#define KCP_PRIVATE_SEGMENT_BUFFER (1u << 0)
#define KCP_DISPATCH_PTR           (1u << 1)
#define KCP_QUEUE_PTR              (1u << 2)
#define KCP_KERNARG_SEGMENT_PTR    (1u << 3)
#define KCP_DISPATCH_ID            (1u << 4)
#define KCP_FLAT_SCRATCH_INIT      (1u << 5)
#define KCP_PRIVATE_SEGMENT_SIZE   (1u << 6)
#define KCP_WAVE32                 (1u << 10)
#define KCP_DYNAMIC_STACK          (1u << 11)

#ifndef EM_AMDGPU
#define EM_AMDGPU 224
#endif

struct pm4_kernel {
	pm4_buf  image;          /* loaded PT_LOAD segments */
	uint64_t entry;          /* GPU address of the first instruction */
	uint32_t rsrc1, rsrc2;
	uint32_t kernarg_size;
	uint32_t user_data[16];  /* template; kernarg pointer patched per dispatch */
	int      kernarg_idx;    /* index of kernarg lo in user_data, or -1 */
	char     name[64];
};

struct pm4_ctx {
	uint32_t node;
	HsaQueueResource q;
	int      have_queue;
	pm4_buf  ring, ibpool, kargs, fence;
	uint32_t wptr;           /* pending write pointer, dwords */
	uint64_t next_id;        /* next fence id to hand out (starts at 1) */
	uint64_t done_id;        /* highest fence id known retired */
	unsigned timeout_ms;
	uint32_t cu_mask;        /* PM4_CU_MASK: per-SE CU enable mask, 0 = leave to KFD */
	int      debug, gpu_ts, wedged, force_nonpaged;
};

/* ---- helpers ----------------------------------------------------------- */
static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int env_int(const char *name, int def)
{
	const char *v = getenv(name);
	return (v && *v) ? atoi(v) : def;
}

#define DBG(ctx, ...) do { if ((ctx)->debug) fprintf(stderr, "pm4: " __VA_ARGS__); } while (0)
#define ERR(...) fprintf(stderr, "pm4: error: " __VA_ARGS__)

const char *pm4_strerror(int err)
{
	switch (err) {
	case PM4_OK:          return "ok";
	case PM4_ERR_ARG:     return "bad argument";
	case PM4_ERR_KFD:     return "KFD call failed";
	case PM4_ERR_NODEV:   return "no usable gfx7 GPU node";
	case PM4_ERR_NOMEM:   return "out of memory";
	case PM4_ERR_IO:      return "cannot read code object";
	case PM4_ERR_ELF:     return "not a usable amdgcn code object";
	case PM4_ERR_NOSYM:   return "kernel symbol not found";
	case PM4_ERR_UNSUP:   return "kernel needs an unsupported feature";
	case PM4_ERR_TIMEOUT: return "timeout";
	case PM4_ERR_WEDGED:  return "context wedged by an earlier timeout";
	default:              return "unknown error";
	}
}

static const char *op_name(unsigned op)
{
	switch (op) {
	case IT_NOP:             return "NOP";
	case IT_DISPATCH_DIRECT: return "DISPATCH_DIRECT";
	case IT_WAIT_REG_MEM:    return "WAIT_REG_MEM";
	case IT_INDIRECT_BUFFER: return "INDIRECT_BUFFER";
	case IT_RELEASE_MEM:     return "RELEASE_MEM";
	case IT_ACQUIRE_MEM:     return "ACQUIRE_MEM";
	case IT_SET_SH_REG:      return "SET_SH_REG";
	default:                 return "?";
	}
}

static void dump_stream(const char *what, const uint32_t *p, unsigned ndw)
{
	unsigned i = 0;

	fprintf(stderr, "pm4: -- %s (%u dwords)\n", what, ndw);
	while (i < ndw) {
		uint32_t h = p[i];
		unsigned n, j;

		if (h == CMD_NOP_TYPE_3) {
			fprintf(stderr, "pm4:   [%3u] %08x  NOP(pad)\n", i, h);
			i++;
			continue;
		}
		n = ((h >> 16) & 0x3FFF) + 2;
		fprintf(stderr, "pm4:   [%3u] %08x  %s", i, h, op_name((h >> 8) & 0xFF));
		if (((h >> 8) & 0xFF) == IT_SET_SH_REG && i + 1 < ndw)
			fprintf(stderr, " reg 0x%04x", p[i + 1] + PERSISTENT_SPACE_START);
		fputc('\n', stderr);
		for (j = 1; j < n && i + j < ndw; j++)
			fprintf(stderr, "pm4:         %08x\n", p[i + j]);
		i += n;
	}
}

/* ---- packet builders (append to p, return new dword count) ------------- */
static unsigned pk_acquire_mem(uint32_t *p, unsigned n, uint32_t coher_cntl)
{
	p[n++] = PM4_HDR(IT_ACQUIRE_MEM, 7);
	p[n++] = coher_cntl;          /* coher_cntl:31, engine:1 = PFP(0) */
	p[n++] = 0xFFFFFFFFu;         /* coher_size */
	p[n++] = 0;                   /* coher_size_hi */
	p[n++] = 0;                   /* coher_base_lo */
	p[n++] = 0;                   /* coher_base_hi */
	p[n++] = 4;                   /* poll_interval */
	return n;
}

static unsigned pk_set_sh_reg(uint32_t *p, unsigned n, uint32_t reg,
			      const uint32_t *vals, unsigned count)
{
	p[n++] = PM4_HDR(IT_SET_SH_REG, 2 + count);
	p[n++] = reg - PERSISTENT_SPACE_START;
	memcpy(&p[n], vals, count * 4);
	return n + count;
}

/* RELEASE_MEM (CI layout): end-of-pipe event, L2 write-back + invalidate,
 * then write `data` (or the GPU clock) to `addr` through the memory
 * controller, bypassing the GPU caches. */
static unsigned pk_release_mem(uint32_t *p, unsigned n, uint64_t addr,
			       uint64_t data, unsigned data_sel)
{
	p[n++] = PM4_HDR(IT_RELEASE_MEM, 7);
	p[n++] = 0x14u                /* event_type */
	       | (5u << 8)            /* event_index = EVENT_WRITE_EOP */
	       | (1u << 15)           /* l2_wb */
	       | (1u << 17)           /* l2_inv */
	       | (0u << 24)           /* atc = 0 (dGPU-style node) */
	       | (2u << 25);          /* cache_policy = BYPASS */
	p[n++] = (0u << 16)           /* dst_sel = memory controller */
	       | (RM_INT_NONE_WAIT_WC << 24)
	       | ((uint32_t)data_sel << 29);
	p[n++] = (uint32_t)addr & ~3u;
	p[n++] = (uint32_t)(addr >> 32);
	p[n++] = (uint32_t)data;
	p[n++] = (uint32_t)(data >> 32);
	return n;
}

static unsigned pk_wait_mem_eq(uint32_t *p, unsigned n, uint64_t addr, uint32_t ref)
{
	p[n++] = PM4_HDR(IT_WAIT_REG_MEM, 7);
	p[n++] = 3u | (1u << 4);      /* function = equal, mem_space = memory */
	p[n++] = (uint32_t)addr & ~3u;
	p[n++] = (uint32_t)(addr >> 32);
	p[n++] = ref;
	p[n++] = 0xFFFFFFFFu;         /* mask */
	p[n++] = 4u | (1u << 31);     /* poll_interval, optimize_ace_offload_mode */
	return n;
}

static unsigned pk_indirect(uint32_t *p, unsigned n, uint64_t ib, unsigned ib_dw)
{
	p[n++] = PM4_HDR(IT_INDIRECT_BUFFER, 4);
	p[n++] = (uint32_t)ib & ~3u;
	p[n++] = (uint32_t)(ib >> 32) & 0xFFFFu;
	p[n++] = (ib_dw & 0xFFFFFu) | (1u << 23) /* valid */ | (2u << 28) /* BYPASS */;
	return n;
}

/* ---- ring -------------------------------------------------------------- */
static uint32_t ring_rptr(pm4_ctx *ctx)
{
	return *(volatile uint32_t *)ctx->q.Queue_read_ptr;
}

/* Copy packets into the ring (kfdtest BaseQueue::PlacePacket), waiting a
 * bounded time for the CP to free space. */
static int ring_place(pm4_ctx *ctx, const uint32_t *pkt, unsigned ndw)
{
	uint32_t *ring = ctx->ring.cpu;
	uint64_t deadline = now_ns() + (uint64_t)ctx->timeout_ms * 1000000ull;
	unsigned need = ndw, pad = 0;

	if (ctx->wptr + ndw > RING_DWORDS) {
		pad = RING_DWORDS - ctx->wptr;
		need += pad;
	}
	for (;;) {
		unsigned avail = (ring_rptr(ctx) - 1u - ctx->wptr + RING_DWORDS) % RING_DWORDS;

		if (avail >= need)
			break;
		if (now_ns() > deadline)
			return PM4_ERR_TIMEOUT;
		sched_yield();
	}
	if (pad) {
		while (ctx->wptr < RING_DWORDS)
			ring[ctx->wptr++] = CMD_NOP_TYPE_3;
		ctx->wptr = 0;
	}
	memcpy(&ring[ctx->wptr], pkt, ndw * 4);
	ctx->wptr = (ctx->wptr + ndw) % RING_DWORDS;
	return PM4_OK;
}

/* kfdtest PM4Queue::SubmitPacket, pre-Vega path: 32-bit wptr + doorbell. */
static void ring_submit(pm4_ctx *ctx)
{
	__sync_synchronize();
	*(volatile uint32_t *)ctx->q.Queue_write_ptr = ctx->wptr;
	__sync_synchronize();
	*(volatile uint32_t *)ctx->q.Queue_DoorBell = ctx->wptr;
}

/* ---- memory ------------------------------------------------------------ */
int pm4_alloc(pm4_ctx *ctx, size_t bytes, unsigned flags, pm4_buf *out)
{
	HsaMemFlags mf;
	HsaMemMapFlags mapf;
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	void *addr = NULL;
	HSAuint32 node;

	if (!ctx || !out || !bytes)
		return PM4_ERR_ARG;
	memset(out, 0, sizeof(*out));
	bytes = (bytes + pg - 1) & ~(pg - 1);

	/* Same flags as kfdtest's HsaMemoryBuffer for host-accessible memory. */
	memset(&mf, 0, sizeof(mf));
	mf.ui32.PageSize = HSA_PAGE_SIZE_4KB;
	mf.ui32.HostAccess = 1;
	/* default: pageable (userptr) memory, the path kfdtest proves; NonPaged
	 * asks KFD for a pinned GTT buffer object instead. */
	mf.ui32.NonPaged = (flags & PM4_ALLOC_NONPAGED) || ctx->force_nonpaged;
	mf.ui32.NoNUMABind = 1;
	mf.ui32.ExecuteAccess = !!(flags & PM4_ALLOC_EXEC);
	mf.ui32.Uncached = !!(flags & PM4_ALLOC_UNCACHED);

	if (hsaKmtAllocMemory(ctx->node, bytes, mf, &addr) != HSAKMT_STATUS_SUCCESS || !addr) {
		ERR("hsaKmtAllocMemory(%zu bytes) failed\n", bytes);
		return PM4_ERR_NOMEM;
	}
	memset(&mapf, 0, sizeof(mapf));
	node = ctx->node;
	if (hsaKmtMapMemoryToGPUNodes(addr, bytes, NULL, mapf, 1, &node) != HSAKMT_STATUS_SUCCESS) {
		ERR("hsaKmtMapMemoryToGPUNodes(%zu bytes) failed\n", bytes);
		hsaKmtFreeMemory(addr, bytes);
		return PM4_ERR_KFD;
	}
	if (flags & PM4_ALLOC_ZERO)
		memset(addr, 0, bytes);
	out->cpu = addr;
	out->gpu = (uint64_t)(uintptr_t)addr;
	out->size = bytes;
	return PM4_OK;
}

int pm4_free(pm4_ctx *ctx, pm4_buf *buf)
{
	int rc = PM4_OK;

	if (!ctx || !buf)
		return PM4_ERR_ARG;
	if (!buf->cpu)
		return PM4_OK;
	if (hsaKmtUnmapMemoryToGPU(buf->cpu) != HSAKMT_STATUS_SUCCESS)
		rc = PM4_ERR_KFD;
	if (hsaKmtFreeMemory(buf->cpu, buf->size) != HSAKMT_STATUS_SUCCESS)
		rc = PM4_ERR_KFD;
	memset(buf, 0, sizeof(*buf));
	return rc;
}

int pm4_import_dmabuf(pm4_ctx *ctx, int fd, size_t bytes, pm4_buf *out)
{
	HsaGraphicsResourceInfo info;
	HsaMemMapFlags mapf;
	HSAuint32 node;

	if (!ctx || !out || fd < 0 || !bytes)
		return PM4_ERR_ARG;
	memset(out, 0, sizeof(*out));
	memset(&info, 0, sizeof(info));
	node = ctx->node;
	if (hsaKmtRegisterGraphicsHandleToNodes((HSAuint64)fd, &info, 1, &node) != HSAKMT_STATUS_SUCCESS ||
	    !info.MemoryAddress) {
		ERR("hsaKmtRegisterGraphicsHandleToNodes(fd %d) failed\n", fd);
		return PM4_ERR_KFD;
	}
	if (info.SizeInBytes < bytes) {
		ERR("dmabuf is %llu bytes, %zu wanted\n", (unsigned long long)info.SizeInBytes, bytes);
		hsaKmtDeregisterMemory(info.MemoryAddress);
		return PM4_ERR_ARG;
	}
	memset(&mapf, 0, sizeof(mapf));
	node = ctx->node;
	if (hsaKmtMapMemoryToGPUNodes(info.MemoryAddress, info.SizeInBytes, NULL, mapf, 1, &node) !=
	    HSAKMT_STATUS_SUCCESS) {
		ERR("hsaKmtMapMemoryToGPUNodes(import, %llu bytes) failed\n",
		    (unsigned long long)info.SizeInBytes);
		hsaKmtDeregisterMemory(info.MemoryAddress);
		return PM4_ERR_KFD;
	}
	out->cpu = NULL;
	out->gpu = (uint64_t)(uintptr_t)info.MemoryAddress;
	out->size = (size_t)info.SizeInBytes;
	return PM4_OK;
}

int pm4_release_import(pm4_ctx *ctx, pm4_buf *buf)
{
	int rc = PM4_OK;
	void *addr;

	if (!ctx || !buf)
		return PM4_ERR_ARG;
	if (!buf->gpu)
		return PM4_OK;
	addr = (void *)(uintptr_t)buf->gpu;
	if (hsaKmtUnmapMemoryToGPU(addr) != HSAKMT_STATUS_SUCCESS)
		rc = PM4_ERR_KFD;
	if (hsaKmtDeregisterMemory(addr) != HSAKMT_STATUS_SUCCESS)
		rc = PM4_ERR_KFD;
	memset(buf, 0, sizeof(*buf));
	return rc;
}

/* ---- init / shutdown --------------------------------------------------- */
int pm4_init(pm4_ctx **pctx)
{
	HsaSystemProperties sys;
	HsaNodeProperties np;
	pm4_ctx *ctx;
	int want_node = env_int("PM4_NODE", -1);
	int found = 0, rc;
	uint32_t i;

	if (!pctx)
		return PM4_ERR_ARG;
	*pctx = NULL;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return PM4_ERR_NOMEM;
	ctx->debug = env_int("PM4_DEBUG", 0);
	ctx->gpu_ts = env_int("PM4_GPU_TS", 0);
	ctx->force_nonpaged = env_int("PM4_NONPAGED", 0);
	ctx->cu_mask = getenv("PM4_CU_MASK") ? (uint32_t)strtoul(getenv("PM4_CU_MASK"), NULL, 0) : 0;
	ctx->timeout_ms = (unsigned)env_int("PM4_TIMEOUT_MS", DEFAULT_TIMEOUT_MS);
	ctx->next_id = 1;

	if (hsaKmtOpenKFD() != HSAKMT_STATUS_SUCCESS) {
		ERR("hsaKmtOpenKFD failed (is /dev/kfd accessible?)\n");
		free(ctx);
		return PM4_ERR_KFD;
	}
	if (hsaKmtAcquireSystemProperties(&sys) != HSAKMT_STATUS_SUCCESS) {
		ERR("hsaKmtAcquireSystemProperties failed\n");
		rc = PM4_ERR_KFD;
		goto fail_close;
	}
	for (i = 0; i < sys.NumNodes && !found; i++) {
		if (want_node >= 0 && (uint32_t)want_node != i)
			continue;
		if (hsaKmtGetNodeProperties(i, &np) != HSAKMT_STATUS_SUCCESS)
			continue;
		if (!np.NumFComputeCores)
			continue;
		if (np.EngineId.ui32.Major != 7) {
			ERR("node %u is gfx%u, only gfx7 is supported\n", i, np.EngineId.ui32.Major);
			continue;
		}
		ctx->node = i;
		found = 1;
	}
	if (!found) {
		ERR("no gfx7 GPU node found\n");
		rc = PM4_ERR_NODEV;
		goto fail_props;
	}
	DBG(ctx, "node %u: gfx %u.%u.%u, %u SIMDs, %u arrays x %u CUs\n", ctx->node,
	    np.EngineId.ui32.Major, np.EngineId.ui32.Minor, np.EngineId.ui32.Stepping,
	    np.NumFComputeCores, np.NumShaderBanks, np.NumCUPerArray);

	/* Ring and IBs: uncached + executable, as kfdtest allocates them. */
	if ((rc = pm4_alloc(ctx, RING_BYTES, PM4_ALLOC_ZERO | PM4_ALLOC_EXEC | PM4_ALLOC_UNCACHED, &ctx->ring)) ||
	    (rc = pm4_alloc(ctx, PM4_SLOTS * IB_BYTES, PM4_ALLOC_ZERO | PM4_ALLOC_EXEC | PM4_ALLOC_UNCACHED, &ctx->ibpool)) ||
	    (rc = pm4_alloc(ctx, PM4_SLOTS * PM4_MAX_KERNARG, PM4_ALLOC_ZERO, &ctx->kargs)) ||
	    (rc = pm4_alloc(ctx, FENCE_BYTES, PM4_ALLOC_ZERO, &ctx->fence)))
		goto fail_mem;

	memset(&ctx->q, 0, sizeof(ctx->q));
	if (hsaKmtCreateQueue(ctx->node, HSA_QUEUE_COMPUTE, 100, HSA_QUEUE_PRIORITY_NORMAL,
			      ctx->ring.cpu, RING_BYTES, NULL, &ctx->q) != HSAKMT_STATUS_SUCCESS ||
	    !ctx->q.Queue_read_ptr || !ctx->q.Queue_write_ptr || !ctx->q.Queue_DoorBell) {
		ERR("hsaKmtCreateQueue(PM4 compute) failed\n");
		rc = PM4_ERR_KFD;
		goto fail_mem;
	}
	ctx->have_queue = 1;
	DBG(ctx, "PM4 queue id %llu, ring %p, fence %p\n",
	    (unsigned long long)ctx->q.QueueId, ctx->ring.cpu, ctx->fence.cpu);
	*pctx = ctx;
	return PM4_OK;

fail_mem:
	pm4_free(ctx, &ctx->fence);
	pm4_free(ctx, &ctx->kargs);
	pm4_free(ctx, &ctx->ibpool);
	pm4_free(ctx, &ctx->ring);
fail_props:
	hsaKmtReleaseSystemProperties();
fail_close:
	hsaKmtCloseKFD();
	free(ctx);
	return rc;
}

int pm4_shutdown(pm4_ctx *ctx)
{
	int rc = PM4_OK;

	if (!ctx)
		return PM4_ERR_ARG;
	/* Give outstanding work a bounded chance to retire first. */
	if (!ctx->wedged && ctx->next_id > 1)
		pm4_wait(ctx, (pm4_fence)(ctx->next_id - 1), ctx->timeout_ms);
	if (ctx->have_queue && hsaKmtDestroyQueue(ctx->q.QueueId) != HSAKMT_STATUS_SUCCESS) {
		ERR("hsaKmtDestroyQueue failed\n");
		rc = PM4_ERR_KFD;
	}
	pm4_free(ctx, &ctx->fence);
	pm4_free(ctx, &ctx->kargs);
	pm4_free(ctx, &ctx->ibpool);
	pm4_free(ctx, &ctx->ring);
	hsaKmtReleaseSystemProperties();
	hsaKmtCloseKFD();
	free(ctx);
	return rc;
}

/* ---- code object loading ----------------------------------------------- */
static int read_file(const char *path, uint8_t **data, size_t *len)
{
	FILE *f = fopen(path, "rb");
	long sz;
	uint8_t *buf;

	if (!f)
		return PM4_ERR_IO;
	if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		return PM4_ERR_IO;
	}
	buf = malloc((size_t)sz);
	if (!buf) {
		fclose(f);
		return PM4_ERR_NOMEM;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return PM4_ERR_IO;
	}
	fclose(f);
	*data = buf;
	*len = (size_t)sz;
	return PM4_OK;
}

#define IN_FILE(off, sz) ((uint64_t)(off) <= len && (uint64_t)(sz) <= len - (uint64_t)(off))

/* Find a defined symbol by name in .symtab or .dynsym. */
static int elf_find_sym(const uint8_t *d, size_t len, const Elf64_Ehdr *eh,
			const char *name, Elf64_Sym *out)
{
	const Elf64_Shdr *sh = (const Elf64_Shdr *)(d + eh->e_shoff);
	unsigned i;

	for (i = 0; i < eh->e_shnum; i++) {
		const Elf64_Shdr *str;
		uint64_t j, nsym;

		if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM)
			continue;
		if (sh[i].sh_link >= eh->e_shnum || sh[i].sh_entsize != sizeof(Elf64_Sym) ||
		    !IN_FILE(sh[i].sh_offset, sh[i].sh_size))
			continue;
		str = &sh[sh[i].sh_link];
		if (!IN_FILE(str->sh_offset, str->sh_size) || !str->sh_size ||
		    d[str->sh_offset + str->sh_size - 1] != 0)
			continue;
		nsym = sh[i].sh_size / sizeof(Elf64_Sym);
		for (j = 0; j < nsym; j++) {
			const Elf64_Sym *s = (const Elf64_Sym *)(d + sh[i].sh_offset) + j;

			if (s->st_shndx == SHN_UNDEF || s->st_name >= str->sh_size)
				continue;
			if (!strcmp((const char *)d + str->sh_offset + s->st_name, name)) {
				*out = *s;
				return PM4_OK;
			}
		}
	}
	return PM4_ERR_NOSYM;
}

int pm4_load(pm4_ctx *ctx, const char *path, const char *name, pm4_kernel **pkern)
{
	uint8_t *d = NULL;
	size_t len = 0;
	const Elf64_Ehdr *eh;
	const Elf64_Phdr *ph;
	struct kernel_descriptor kd;
	Elf64_Sym kdsym;
	pm4_kernel *k = NULL;
	uint64_t span = 0, entry_va;
	char kdname[80];
	unsigned i, idx, mach;
	int rc;

	if (!ctx || !path || !name || !pkern || strlen(name) >= sizeof(k->name))
		return PM4_ERR_ARG;
	*pkern = NULL;
	if ((rc = read_file(path, &d, &len))) {
		ERR("%s: %s\n", path, strerror(errno));
		return rc;
	}
	rc = PM4_ERR_ELF;
	eh = (const Elf64_Ehdr *)d;
	if (len < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) ||
	    eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB ||
	    eh->e_machine != EM_AMDGPU || eh->e_type != ET_DYN) {
		ERR("%s: not a linked amdgcn ELF64 code object\n", path);
		goto out;
	}
	if (eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_shentsize != sizeof(Elf64_Shdr) ||
	    !IN_FILE(eh->e_phoff, (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr)) ||
	    !IN_FILE(eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr))) {
		ERR("%s: corrupt ELF headers\n", path);
		goto out;
	}
	mach = eh->e_flags & 0xFF;      /* EF_AMDGPU_MACH: 0x22 gfx700 .. 0x26 gfx705 */
	if (mach < 0x22 || mach > 0x26) {
		ERR("%s: built for EF_AMDGPU_MACH 0x%x, need gfx7 (-mcpu=hawaii)\n", path, mach);
		rc = PM4_ERR_UNSUP;
		goto out;
	}

	ph = (const Elf64_Phdr *)(d + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		if (!IN_FILE(ph[i].p_offset, ph[i].p_filesz) || ph[i].p_filesz > ph[i].p_memsz ||
		    ph[i].p_vaddr > (1u << 30) || ph[i].p_memsz > (1u << 30)) {
			ERR("%s: bad PT_LOAD segment\n", path);
			goto out;
		}
		if (ph[i].p_vaddr + ph[i].p_memsz > span)
			span = ph[i].p_vaddr + ph[i].p_memsz;
	}
	/* Kernels that reference globals need dynamic relocations; we do not apply any. */
	{
		const Elf64_Shdr *sh = (const Elf64_Shdr *)(d + eh->e_shoff);

		for (i = 0; i < eh->e_shnum; i++)
			if ((sh[i].sh_type == SHT_RELA || sh[i].sh_type == SHT_REL) && sh[i].sh_size) {
				ERR("%s: has relocations (global variables?), not supported\n", path);
				rc = PM4_ERR_UNSUP;
				goto out;
			}
	}

	snprintf(kdname, sizeof(kdname), "%s.kd", name);
	if (elf_find_sym(d, len, eh, kdname, &kdsym)) {
		ERR("%s: symbol %s not found\n", path, kdname);
		rc = PM4_ERR_NOSYM;
		goto out;
	}
	if (!span || kdsym.st_value > span || span - kdsym.st_value < sizeof(kd)) {
		ERR("%s: %s lies outside the loaded image\n", path, kdname);
		goto out;
	}

	k = calloc(1, sizeof(*k));
	if (!k) {
		rc = PM4_ERR_NOMEM;
		goto out;
	}
	strcpy(k->name, name);
	if ((rc = pm4_alloc(ctx, span, PM4_ALLOC_ZERO | PM4_ALLOC_EXEC, &k->image)))
		goto out;
	for (i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD)
			memcpy((uint8_t *)k->image.cpu + ph[i].p_vaddr, d + ph[i].p_offset, ph[i].p_filesz);

	memcpy(&kd, (uint8_t *)k->image.cpu + kdsym.st_value, sizeof(kd));
	entry_va = kdsym.st_value + (uint64_t)kd.kernel_code_entry_byte_offset;
	rc = PM4_ERR_ELF;
	if (entry_va >= span) {
		ERR("%s: kernel entry outside the image\n", name);
		goto out;
	}
	k->entry = k->image.gpu + entry_va;
	if (k->entry & 0xFF) {
		ERR("%s: entry %#llx is not 256-byte aligned\n", name, (unsigned long long)k->entry);
		goto out;
	}

	/* Hardware rules: no scratch, wave64 only; bridge rules: no dispatch/queue ptr. */
	rc = PM4_ERR_UNSUP;
	if (kd.private_segment_fixed_size || (kd.compute_pgm_rsrc2 & RSRC2_SCRATCH_EN) ||
	    (kd.kernel_code_properties & (KCP_FLAT_SCRATCH_INIT | KCP_PRIVATE_SEGMENT_SIZE | KCP_DYNAMIC_STACK))) {
		ERR("%s: kernel needs scratch (private_segment_fixed_size %u, properties %#x); "
		    "this GPU has no scratch\n", name, kd.private_segment_fixed_size, kd.kernel_code_properties);
		goto out;
	}
	if (kd.kernel_code_properties & (KCP_DISPATCH_PTR | KCP_QUEUE_PTR | KCP_DISPATCH_ID | KCP_WAVE32)) {
		ERR("%s: kernel wants dispatch ptr/queue ptr/dispatch id/wave32 (properties %#x); "
		    "use only __builtin_amdgcn_workitem_id_x/workgroup_id_x\n", name, kd.kernel_code_properties);
		goto out;
	}
	if (kd.group_segment_fixed_size > 64 * 1024) {
		ERR("%s: LDS size %u exceeds 64 KB\n", name, kd.group_segment_fixed_size);
		goto out;
	}
	if (kd.kernarg_size > PM4_MAX_KERNARG) {
		ERR("%s: kernarg size %u exceeds %u\n", name, kd.kernarg_size, PM4_MAX_KERNARG);
		goto out;
	}

	/* User SGPR layout of the amdhsa ABI: s[0:3] private segment buffer
	 * (zero: no scratch), then the kernarg segment pointer. */
	idx = 0;
	k->kernarg_idx = -1;
	if (kd.kernel_code_properties & KCP_PRIVATE_SEGMENT_BUFFER)
		idx += 4;
	if (kd.kernel_code_properties & KCP_KERNARG_SEGMENT_PTR) {
		k->kernarg_idx = (int)idx;
		idx += 2;
	}
	if (idx != RSRC2_USER_SGPR(kd.compute_pgm_rsrc2)) {
		ERR("%s: user SGPR count %u does not match the layout we can provide (%u)\n",
		    name, RSRC2_USER_SGPR(kd.compute_pgm_rsrc2), idx);
		goto out;
	}
	k->kernarg_size = kd.kernarg_size;
	k->rsrc1 = kd.compute_pgm_rsrc1;
	k->rsrc2 = (kd.compute_pgm_rsrc2 & ~RSRC2_LDS_SIZE_MASK) |
		   (((kd.group_segment_fixed_size + 511u) / 512u) << RSRC2_LDS_SIZE_SHIFT);

	DBG(ctx, "loaded %s from %s: entry %#llx rsrc1 %#x rsrc2 %#x kernarg %u bytes lds %u props %#x\n",
	    name, path, (unsigned long long)k->entry, k->rsrc1, k->rsrc2, k->kernarg_size,
	    kd.group_segment_fixed_size, kd.kernel_code_properties);
	*pkern = k;
	k = NULL;
	rc = PM4_OK;
out:
	if (k) {
		pm4_free(ctx, &k->image);
		free(k);
	}
	free(d);
	return rc;
}

int pm4_unload(pm4_ctx *ctx, pm4_kernel *kern)
{
	int rc;

	if (!ctx || !kern)
		return PM4_ERR_ARG;
	rc = pm4_free(ctx, &kern->image);
	free(kern);
	return rc;
}

/* ---- dispatch ---------------------------------------------------------- */
static uint64_t *ts_slot(pm4_ctx *ctx, unsigned slot)
{
	return (uint64_t *)((uint8_t *)ctx->fence.cpu + TS_OFFSET + slot * 16u);
}

pm4_fence pm4_dispatch(pm4_ctx *ctx, pm4_kernel *kern, uint64_t grid_x,
		       uint32_t block_x, const void *kernarg, size_t kernarg_size)
{
	uint32_t ib[IB_DWORDS], rp[32], v[16];
	uint64_t groups, id, ib_gpu, karg_gpu, nop_gpu;
	unsigned n = 0, rn = 0, slot, nop_idx;
	uint8_t *karg_cpu;
	int rc;

	if (!ctx || !kern || !grid_x || !block_x || block_x > PM4_MAX_BLOCK)
		return PM4_ERR_ARG;
	if (kernarg_size > PM4_MAX_KERNARG || (kernarg_size && !kernarg) ||
	    kernarg_size < kern->kernarg_size)
		return PM4_ERR_ARG;
	if (ctx->wedged)
		return PM4_ERR_WEDGED;
	groups = (grid_x + block_x - 1) / block_x;
	if (groups > 0xFFFFFFFFull)
		return PM4_ERR_ARG;

	id = ctx->next_id;
	slot = (unsigned)(id % PM4_SLOTS);
	/* The slot's IB and kernarg block are free once its previous user retired. */
	if (id > PM4_SLOTS && (rc = pm4_wait(ctx, (pm4_fence)(id - PM4_SLOTS), ctx->timeout_ms)))
		return rc;

	ib_gpu = ctx->ibpool.gpu + (uint64_t)slot * IB_BYTES;
	karg_gpu = ctx->kargs.gpu + (uint64_t)slot * PM4_MAX_KERNARG;
	karg_cpu = (uint8_t *)ctx->kargs.cpu + (size_t)slot * PM4_MAX_KERNARG;
	if (kernarg_size)
		memcpy(karg_cpu, kernarg, kernarg_size);

	/* Submission start: invalidate L1/L2/K$/I$ so the GPU sees CPU writes. */
	n = pk_acquire_mem(ib, n, COHER_PRE);
	if (ctx->gpu_ts) {
		ts_slot(ctx, slot)[0] = 0;
		ts_slot(ctx, slot)[1] = 0;
		n = pk_release_mem(ib, n, (uint64_t)(uintptr_t)&ts_slot(ctx, slot)[0], 0, RM_DATA_GPU_CLOCK);
	}

	/* COMPUTE_START_X.. : start 0,0,0; threads per group; pipelinestat, perfcount off */
	v[0] = 0; v[1] = 0; v[2] = 0; v[3] = block_x; v[4] = 1; v[5] = 1; v[6] = 0; v[7] = 0;
	n = pk_set_sh_reg(ib, n, mmCOMPUTE_START_X, v, 8);

	/* PGM_LO/HI: 256-byte aligned code address >> 8; no ATC on this node */
	v[0] = (uint32_t)(kern->entry >> 8);
	v[1] = (uint32_t)(kern->entry >> 40) & 0xFFu;
	n = pk_set_sh_reg(ib, n, mmCOMPUTE_PGM_LO, v, 2);

	v[0] = kern->rsrc1;
	v[1] = kern->rsrc2;
	n = pk_set_sh_reg(ib, n, mmCOMPUTE_PGM_RSRC1, v, 2);

	v[0] = 0;
	n = pk_set_sh_reg(ib, n, mmCOMPUTE_RESOURCE_LIMITS, v, 1);

	if (ctx->cu_mask) {
		/* SE0, SE1, TMPRING_SIZE, SE2, SE3 are consecutive registers. */
		v[0] = ctx->cu_mask; v[1] = ctx->cu_mask; v[2] = 0; v[3] = ctx->cu_mask; v[4] = ctx->cu_mask;
		n = pk_set_sh_reg(ib, n, mmCOMPUTE_STATIC_THREAD_MGMT_SE0, v, 5);
	} else {
		v[0] = 0;       /* no scratch on this GPU */
		n = pk_set_sh_reg(ib, n, mmCOMPUTE_TMPRING_SIZE, v, 1);
	}

	v[0] = 0; v[1] = 0; v[2] = 0; v[3] = 0;   /* RESTART_X/Y/Z, THREAD_TRACE_ENABLE */
	n = pk_set_sh_reg(ib, n, mmCOMPUTE_RESTART_X, v, 4);

	memset(v, 0, sizeof(v));
	if (kern->kernarg_idx >= 0) {
		v[kern->kernarg_idx] = (uint32_t)karg_gpu;
		v[kern->kernarg_idx + 1] = (uint32_t)(karg_gpu >> 32);
	}
	n = pk_set_sh_reg(ib, n, mmCOMPUTE_USER_DATA_0, v, 16);

	ib[n++] = PM4_HDR(IT_DISPATCH_DIRECT, 5);
	ib[n++] = (uint32_t)groups;
	ib[n++] = 1;
	ib[n++] = 1;
	ib[n++] = DISPATCH_INITIATOR;

	/* Wait for the dispatch to drain. kfdtest replaces EVENT_WRITE
	 * CS_PARTIAL_FLUSH with RELEASE_MEM (no interrupt) + WAIT_REG_MEM on a
	 * fence dword stored in a NOP inside the IB; we do the same. */
	ib[n++] = PM4_HDR(IT_NOP, 2);
	nop_idx = n;
	ib[n++] = 0;
	nop_gpu = ib_gpu + (uint64_t)nop_idx * 4u;
	n = pk_release_mem(ib, n, nop_gpu, 0xdeadbeefu, RM_DATA_32);
	n = pk_wait_mem_eq(ib, n, nop_gpu, 0xdeadbeefu);

	/* The L1 is not coherent between dispatches: drop TCL1 + K$. */
	n = pk_acquire_mem(ib, n, COHER_POST);

	if (n > IB_DWORDS)
		return PM4_ERR_ARG;     /* cannot happen; guards future edits */
	memcpy((uint8_t *)ctx->ibpool.cpu + (size_t)slot * IB_BYTES, ib, n * 4u);

	rn = pk_indirect(rp, rn, ib_gpu, n);
	if (ctx->gpu_ts)
		rn = pk_release_mem(rp, rn, (uint64_t)(uintptr_t)&ts_slot(ctx, slot)[1], 0, RM_DATA_GPU_CLOCK);
	/* End of pipe: L2 write-back + invalidate, then the fence the CPU polls. */
	rn = pk_release_mem(rp, rn, ctx->fence.gpu, (uint32_t)id, RM_DATA_32);

	if (ctx->debug) {
		fprintf(stderr, "pm4: dispatch %s fence %llu slot %u: grid %llu block %u groups %llu kernarg %#llx\n",
			kern->name, (unsigned long long)id, slot, (unsigned long long)grid_x, block_x,
			(unsigned long long)groups, (unsigned long long)karg_gpu);
		dump_stream("indirect buffer", ib, n);
		dump_stream("ring", rp, rn);
	}

	if ((rc = ring_place(ctx, rp, rn))) {
		ERR("ring full for %u ms; GPU not consuming packets\n", ctx->timeout_ms);
		ctx->wedged = 1;
		return rc;
	}
	ring_submit(ctx);
	ctx->next_id = id + 1;
	return (pm4_fence)id;
}

static int fence_passed(pm4_ctx *ctx, uint64_t id)
{
	uint32_t cur;

	if (id <= ctx->done_id)
		return 1;
	cur = *(volatile uint32_t *)ctx->fence.cpu;
	/* fence ids are written modulo 2^32, in submission order */
	if ((int32_t)(cur - (uint32_t)id) >= 0) {
		ctx->done_id = id;
		return 1;
	}
	return 0;
}

int pm4_wait(pm4_ctx *ctx, pm4_fence fence, unsigned timeout_ms)
{
	uint64_t start, deadline, t;
	unsigned spins = 0;

	if (!ctx || fence <= 0 || (uint64_t)fence >= ctx->next_id)
		return PM4_ERR_ARG;
	if (fence_passed(ctx, (uint64_t)fence))
		return PM4_OK;
	start = now_ns();
	deadline = start + (uint64_t)timeout_ms * 1000000ull;
	for (;;) {
		if (fence_passed(ctx, (uint64_t)fence))
			return PM4_OK;
		if ((++spins & 63) && spins < 4096)
			continue;                 /* short busy-poll first */
		t = now_ns();
		if (t >= deadline)
			break;
		if (t - start > 2000000ull) {     /* after 2 ms, sleep between polls */
			struct timespec ts = { 0, 50000 };
			nanosleep(&ts, NULL);
		} else {
			sched_yield();
		}
	}
	if (fence_passed(ctx, (uint64_t)fence))
		return PM4_OK;
	ERR("fence %lld not reached after %u ms (last retired %u, rptr %u, wptr %u)\n",
	    (long long)fence, timeout_ms, *(volatile uint32_t *)ctx->fence.cpu,
	    ring_rptr(ctx), ctx->wptr);
	ctx->wedged = 1;
	return PM4_ERR_TIMEOUT;
}

int pm4_gpu_ticks(pm4_ctx *ctx, pm4_fence fence, uint64_t *t_start, uint64_t *t_end)
{
	unsigned slot;

	if (!ctx || fence <= 0 || (uint64_t)fence >= ctx->next_id || !ctx->gpu_ts)
		return PM4_ERR_ARG;
	if ((uint64_t)fence + PM4_SLOTS < ctx->next_id || !fence_passed(ctx, (uint64_t)fence))
		return PM4_ERR_ARG;
	slot = (unsigned)((uint64_t)fence % PM4_SLOTS);
	if (t_start)
		*t_start = ts_slot(ctx, slot)[0];
	if (t_end)
		*t_end = ts_slot(ctx, slot)[1];
	return PM4_OK;
}
