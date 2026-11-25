/*
 * pm4bridge - run clang-compiled amdgcn (gfx7) kernels on the PS4 Pro GPU
 * through KFD PM4 compute queues.
 *
 * Single-threaded: one pm4_ctx must only be used from one thread at a time.
 */
#ifndef PM4BRIDGE_H
#define PM4BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pm4_ctx pm4_ctx;
typedef struct pm4_kernel pm4_kernel;

/* A fence id (> 0) identifies one dispatch; negative values are PM4_ERR_*. */
typedef int64_t pm4_fence;

/* Host-visible, GPU-mapped buffer. On this GPU cpu == (void *)gpu. */
typedef struct pm4_buf {
	void    *cpu;
	uint64_t gpu;
	size_t   size;   /* rounded up to the page size */
} pm4_buf;

enum {
	PM4_OK            =  0,
	PM4_ERR_ARG       = -1,   /* bad argument */
	PM4_ERR_KFD       = -2,   /* libhsakmt / KFD call failed */
	PM4_ERR_NODEV     = -3,   /* no usable GPU node */
	PM4_ERR_NOMEM     = -4,
	PM4_ERR_IO        = -5,   /* cannot read the code object */
	PM4_ERR_ELF       = -6,   /* not a usable amdgcn code object */
	PM4_ERR_NOSYM     = -7,   /* kernel (or its .kd) not found */
	PM4_ERR_UNSUP     = -8,   /* kernel needs something this GPU/bridge cannot do */
	PM4_ERR_TIMEOUT   = -9,
	PM4_ERR_WEDGED    = -10,  /* an earlier wait timed out; context refuses new work */
};

/* pm4_alloc flags */
#define PM4_ALLOC_ZERO     0x1u   /* zero-fill */
#define PM4_ALLOC_EXEC     0x2u   /* GPU may execute from it */
#define PM4_ALLOC_UNCACHED 0x4u   /* write-combined CPU mapping */
#define PM4_ALLOC_NONPAGED 0x8u   /* pinned GTT buffer instead of pageable (userptr) memory */

#define PM4_MAX_BLOCK      256u   /* clang's default max_flat_workgroup_size */
#define PM4_MAX_KERNARG    512u

int  pm4_init(pm4_ctx **ctx);
int  pm4_shutdown(pm4_ctx *ctx);

int  pm4_alloc(pm4_ctx *ctx, size_t bytes, unsigned flags, pm4_buf *out);
int  pm4_free(pm4_ctx *ctx, pm4_buf *buf);

/* Load kernel `name` from the amdgcn ELF `path` (looks up `name.kd`).
 * Rejects kernels that need scratch, a dispatch/queue pointer or wave32. */
int  pm4_load(pm4_ctx *ctx, const char *path, const char *name, pm4_kernel **kern);
int  pm4_unload(pm4_ctx *ctx, pm4_kernel *kern);

/* Asynchronous 1-D dispatch of ceil(grid_x / block_x) work groups of block_x
 * items. The kernel must bounds-check the last partial group itself.
 * kernarg is copied, so the caller's copy may be reused immediately.
 * Returns a fence id (> 0) or a negative PM4_ERR_*. */
pm4_fence pm4_dispatch(pm4_ctx *ctx, pm4_kernel *kern, uint64_t grid_x,
		       uint32_t block_x, const void *kernarg, size_t kernarg_size);

/* Wait until `fence` (and everything before it) has retired, GPU caches are
 * written back and results are CPU-visible. Never blocks past timeout_ms. */
int  pm4_wait(pm4_ctx *ctx, pm4_fence fence, unsigned timeout_ms);

/* GPU clock counter samples taken just before and just after the dispatch.
 * Only valid when the context was created with PM4_GPU_TS=1 in the
 * environment, after pm4_wait() succeeded, and only for the most recent
 * PM4_SLOTS dispatches. Units are raw GPU clock ticks. */
int  pm4_gpu_ticks(pm4_ctx *ctx, pm4_fence fence, uint64_t *t_start, uint64_t *t_end);

const char *pm4_strerror(int err);

#ifdef __cplusplus
}
#endif
#endif
