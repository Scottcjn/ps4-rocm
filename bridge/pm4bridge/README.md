# pm4bridge

A small C library that runs clang-compiled amdgcn kernels on the PS4 Pro GPU
("gladius", gfx701) through KFD **PM4** compute queues. Stock ROCr cannot
dispatch on this GPU (its MEC firmware never retires AQL packets); PM4 queues
work, so this is the dispatch core for a ROCm bridge.

Everything below was measured on `thebe` (PS4 Pro, kernel 7.3.4 Strawberry +
KFD patches 0001-0005, MEC fw 421, ROCm 6.4.4 libhsakmt in `~/rocm-gfx7`) on
2026-11-25. No sudo is needed: `/dev/kfd` is world read/write.

## Build

    make            # libpm4bridge.a, kernels/*.co, tests/t_*
    make check      # runs every test under `timeout`, logs to logs/

Needs `gcc`, `clang` with the amdgcn target, `ld.lld`, libdrm, libnuma and the
static `libhsakmt.a` (`ROCM=~/rocm-gfx7` by default).

Device code is compiled with (see `Makefile`):

    clang -x cl -cl-std=CL1.2 -target amdgcn-amd-amdhsa -mcpu=hawaii -nogpulib -O2 \
      -mcode-object-version=5 \
      -Xclang -target-feature -Xclang -flat-for-global \
      -Xclang -target-feature -Xclang -enable-ds128 -o k.co k.cl

Every kernel is also compiled to `.s` and checked by `kernels/audit.sh`: no
`flat_` instructions, no `ds_*_b128`, `private_segment_fixed_size 0`. The
build fails if the audit fails.

Link an application with:

    cc app.c -I ~/pm4bridge libpm4bridge.a ~/rocm-gfx7/lib/libhsakmt.a \
       -ldrm_amdgpu -ldrm -lnuma -lpthread -lrt -lm

## API (`pm4bridge.h`)

| Call | What it does |
|---|---|
| `pm4_init(&ctx)` | Opens KFD, picks the gfx7 node, creates one PM4 compute queue. |
| `pm4_alloc(ctx, bytes, flags, &buf)` | Host-visible, GPU-mapped memory. `buf.cpu` and `buf.gpu` are the same address. Flags: `PM4_ALLOC_ZERO`, `_EXEC`, `_UNCACHED`, `_NONPAGED`. |
| `pm4_free(ctx, &buf)` | Unmaps and frees. |
| `pm4_import_dmabuf(ctx, fd, bytes, &buf)` / `pm4_release_import` | Maps a dmabuf (libdrm amdgpu BO) on the GPU. `buf.cpu` is NULL: map the BO with libdrm. The way to get fast (write-combined) memory. |
| `pm4_load(ctx, "k.co", "name", &kern)` | Parses the ELF, loads its segments into executable GPU memory, reads `name.kd`. Rejects kernels that need scratch, a dispatch/queue pointer, wave32, relocations, or a non-256-byte-aligned entry. |
| `pm4_dispatch(ctx, kern, grid_x, block_x, kernarg, size)` | Asynchronous. Runs `ceil(grid_x/block_x)` work groups of `block_x` items. Copies the kernarg block. Returns a fence id (> 0) or a negative `PM4_ERR_*`. |
| `pm4_wait(ctx, fence, timeout_ms)` | Returns `PM4_OK` once that dispatch and all earlier ones retired and results are CPU-visible, or `PM4_ERR_TIMEOUT`. Never blocks past the timeout. |
| `pm4_gpu_ticks(ctx, fence, &t0, &t1)` | GPU clock counter before/after the dispatch (only with `PM4_GPU_TS=1`). |
| `pm4_unload`, `pm4_shutdown`, `pm4_strerror` | Cleanup and error text. |

Kernel rules (hardware and bridge):

- Work-item index: `__builtin_amdgcn_workgroup_id_x() * block + __builtin_amdgcn_workitem_id_x()`.
  There is no `get_global_id`. Pass `block` (and `n`) in the kernarg block.
- The last work group may be partial: the kernel must bounds-check.
- `__global` pointers only, no function calls, no scratch, no global variables.
- `block_x` <= 256 (clang's default `max_flat_workgroup_size`).
- Kernarg struct on the CPU side must match the kernel's argument layout
  (`__attribute__((packed))` structs in `tests/*.c` show how).

Environment variables:

| Variable | Effect |
|---|---|
| `PM4_DEBUG=1` | Dumps every PM4 stream (indirect buffer + ring packets) to stderr. |
| `PM4_GPU_TS=1` | Adds two GPU clock-counter samples per dispatch. |
| `PM4_TIMEOUT_MS` | Internal waits (slot reuse, ring space, shutdown); default 10000. |
| `PM4_NODE` | Force a KFD node index. |
| `PM4_NONPAGED=1` | Use pinned GTT buffers instead of pageable (userptr) memory. |
| `PM4_CU_MASK=0x..` | Experimental: write this CU mask to `STATIC_THREAD_MGMT_SE0..3`. |
| `PM4_KERNELS` | Directory the tests load `.co` files from (default `kernels`). |

## What is sent to the GPU

Per dispatch, one indirect buffer (IB) plus two or three ring packets. The
packet encodings follow kfdtest (`Dispatch.cpp`, `PM4Packet.cpp`), which
passes on this machine.

Indirect buffer:

1. `ACQUIRE_MEM` coher_cntl `0x28C00000` over the whole address space
   (TCL1 + L2 + K$ + I$ invalidate): the GPU L2 does not see CPU writes.
2. `SET_SH_REG` `START_X..` = 0,0,0, `NUM_THREAD_X` = block, 1, 1, pipelinestat 0, perfcount 0
3. `SET_SH_REG` `PGM_LO/HI` = entry >> 8, (entry >> 40) & 0xFF
4. `SET_SH_REG` `PGM_RSRC1/2` from the kernel descriptor, plus `((lds+511)/512) << 15`
5. `SET_SH_REG` `RESOURCE_LIMITS` = 0, `TMPRING_SIZE` = 0, `RESTART_X/Y/Z` = 0
6. `SET_SH_REG` `USER_DATA_0..15` = {0,0,0,0, kernarg_lo, kernarg_hi, 0...}
7. `DISPATCH_DIRECT` groups,1,1 with `DISPATCH_INITIATOR` = 1
8. `NOP`(fence dword) + `RELEASE_MEM` (no interrupt, L2 write-back + invalidate)
   + `WAIT_REG_MEM`: kfdtest's replacement for `EVENT_WRITE CS_PARTIAL_FLUSH`
9. `ACQUIRE_MEM` coher_cntl `0x08400000` (TCL1 + K$ invalidate): the L1 is not
   coherent between dispatches.

Ring: `INDIRECT_BUFFER`, then `RELEASE_MEM` (end of pipe, L2 write-back +
invalidate, cache policy bypass) writing the fence id that `pm4_wait` polls.

## Results (thebe, 2026-11-25, `make check`)

| Test | Result | Numbers |
|---|---|---|
| noop | PASS | ~10 us per dispatch round trip |
| errors | PASS | 20 API-misuse / bad-object cases fail cleanly, including a kernel that needs scratch |
| fill (run twice, different values) | PASS | 1,000,037 elements, 0 mismatches; fill, refill, CPU rewrite + GPU read-modify-write x3 |
| vadd | PASS | 16M floats in 10.8 ms, 0 mismatches (18.6 GB/s at 12 bytes/element) |
| copybw (256 MB) | PASS | 9.0-9.1 GB/s copied = 18.0-18.2 GB/s read+write traffic; read-only 18.2 GB/s; write-only 7.3 GB/s. CPU `memcpy`: 5.25 GB/s |
| matvec 4096x4096 f32 | PASS | bit-exact against the CPU. Row-major: 8.67 ms = 3.87 GFLOP/s, 7.7 GB/s. Column-major: 3.72 ms = 9.01 GFLOP/s, 18.0 GB/s. CPU, 1 thread: 26.8 ms = 1.25 GFLOP/s |
| stress | PASS | 2 x 1000 dispatches, none lost. Sync 10.7 us/dispatch (min 9.8, max 33.6); async 7.6 us/dispatch |
| wide (experimental, `buffer_*_dwordx4`) | PASS | read 18.1 GB/s, copy 8.3 GB/s; data correct |

GPU-side timing: with `PM4_GPU_TS=1` the tick deltas divided by the CPU times
come out at 98-99.4 MHz in every test, so the counter runs at about 100 MHz
(10 ns per tick; inferred from that ratio, not from documentation). At 100 MHz
the GPU-timed numbers are: copy 29.6 ms (9.1 GB/s), read 14.65 ms (18.3 GB/s),
write 36.5 ms (7.35 GB/s), matvec column-major 3.69 ms. They agree with the
CPU-timed numbers to within 1-2 %.

### The bandwidth ceiling

Read bandwidth stops at about **18 GB/s** and write at about **7.3 GB/s**, and
nothing the dispatch side does moves it:

| Variation | Read GB/s | Copy traffic GB/s | Write GB/s |
|---|---|---|---|
| 1 work group (4 waves) for the whole 256 MB | 7.9 | 2.9 | 7.3 |
| 4 work groups (16 waves) | 17.8 | 11.0 | 7.2 |
| 16 work groups | 18.2 | 21.0 | 7.3 |
| 64 / 256 / 1024 / 4096 / 16384 work groups | 18.2 | 18.0-20.0 | 7.3 |
| 16-byte accesses (`dwordx4`) | 18.1 | 16.6-18.8 | - |
| Pinned GTT memory (`PM4_NONPAGED=1`) | 18.2 | 18.0 | 7.3 |
| `PM4_CU_MASK` 0x1 / 0xf / 0x1ff / 0xffffffff | - | 18.0-19.8 | - |

Sixteen waves already saturate it and four waves reach the full write rate, so
this is a memory-path limit, not a compute or dispatch limit (it is the
snooped path of CPU-cached pages: see "VRAM vs GTT" below). It matches the
~16 GB/s seen in llama.cpp generation. Where the limit sits (memory controller
setup, GPU cache path, clocks) is not determined by these tests.

Matvec shows the same thing: with the matrix stored column-major each wave
reads 64 consecutive floats and runs at the 18 GB/s ceiling; row-major, where
the 64 lanes of a wave read 64 different cache lines, gets 7.7 GB/s.

## VRAM vs GTT: the 18 GB/s ceiling is the SNOOPED bit, not the memory pool (2026-11-25)

Question: does the GPU read the 128 MiB VRAM carve-out much faster than GTT
(system) memory? Short answer: **yes, about 9x, but write-combined GTT memory
is exactly as fast as VRAM.** What makes memory slow is the CPU caching
attribute of its pages, not which pool it is in.

Same kernels (`kernels/copybw.cl`), `tests/t_vrambw`, mean of 5 runs, CPU-timed
(GPU-timed with `PM4_GPU_TS=1` agrees within 1-3 %), all data verified:

| Memory (how it was obtained) | Where it lives | Read GB/s | Write GB/s | Copy GB/s |
|---|---|---|---|---|
| `kfd`: `pm4_alloc` (KFD userptr), 256 MB - the old baseline | system RAM, cached | 18.1 | 7.3 | 9.0 |
| `kfdwc`: `pm4_alloc` NONPAGED+UNCACHED, 256 MB | GTT, cached | 18.1 | 7.3 | 9.0 |
| `gtt`: libdrm GTT BO, imported, 256 MB | GTT, cached | 18.1 | 7.3 | 9.0 |
| `vram`: libdrm VRAM BO, imported, 32 MB | **moved to GTT by KFD**, cached | 18.0 | - | - |
| `gttwc`: libdrm GTT BO with `CPU_GTT_USWC`, imported, 256 MB | GTT, write-combined | **167** | **111** | **56** |
| `gttwc`, 512 MB, read only | GTT, write-combined | **168** | - | - |

Mixed copies run at the speed of the slow side: `gttwc` -> `kfd` 7.4 GB/s
(the cached write rate), `kfd` -> `gttwc` 17.6 GB/s (the cached read rate).

Real VRAM can only be measured through Vulkan (`vk/vkbw`, GLSL versions of the
same three kernels, patched Mesa, 16 dispatches per timed submit):

| Vulkan memory type | Where it lives (sysfs + fdinfo) | Read | Write | Copy |
|---|---|---|---|---|
| type 5, HOST_CACHED, 32 MB | GTT, cached | 18.2 | 7.2 | 9.3 |
| type 3, DEVICE_LOCAL+HOST_VISIBLE, 32 MB | **VRAM** (`mem_info_vram_used` +32 MiB per buffer, 0 evicted) | 161 | 107 | 57 |
| same, 48 MB pair / 96 MB read only | VRAM (108.7 of 128 MiB used) | 162 / 164 | 108 | 58 |
| type 2, HOST_VISIBLE not cached, 32 MB | GTT, write-combined (`vram_used` unchanged) | 159 | 110 | 56 |

What this means:

- amdgpu sets `AMDGPU_PTE_SNOOPED` in the GPU page table for pages whose TTM
  caching is "cached" and leaves it clear for write-combined pages and for
  VRAM. On this console a snooped access is ~9x slower for reads and ~15x for
  writes. (The bit is inferred from the amdgpu source and from the behaviour
  above; the page tables were not dumped.)
- **A bigger VRAM carve-out is not needed for bandwidth.** Any GTT buffer
  created with `AMDGPU_GEM_CREATE_CPU_GTT_USWC` gets the fast path, up to the
  GTT size (3.9 GiB here), with the stock 128 MiB carve-out.
- KFD cannot produce such memory by itself: userptr and KFD GTT BOs are
  always cached (`HsaMemFlags.Uncached` changes nothing here), and a VRAM BO
  imported into KFD is migrated to GTT at import/map time (`apu_prefer_gtt`)
  and becomes cached memory. The working route is
  `amdgpu_bo_alloc(GTT, CPU_GTT_USWC)` -> `amdgpu_bo_export(dma_buf_fd)` ->
  `pm4_import_dmabuf()`; see `mem_get()` in `tests/t_vrambw.c`.
- CPU reads from write-combined memory are slow (uncached). Use it for data
  the GPU reads and the CPU only writes (weights), and `memcpy` out in one
  pass when the CPU needs a result.
- 167-176 GB/s may now be limited by the kernel/shader loop and not by
  memory: it was not checked against wave counts.

Run: `./run.sh t_vrambw 120 <MB> <src kind> <dst kind|none> [iters] [reps]`
(`VRAMBW_STEPS=1` prints where each BO lives after every step) and
`vk/run.sh <MB> vram|gtt|gttwc <kind|none> [iters] [reps] [passes]`
(`make vk` builds it; it refuses to run without the patched-Mesa ICD).
Logs: `logs/t_vrambw.log`, `logs/vkbw.log`. No hangs, trouble count 0 throughout.

## Differences from kfdtest and from the Garlic register facts

- **`DISPATCH_INITIATOR`**: kfdtest uses `0x21` (`USE_THREAD_DIMENSIONS`).
  With that bit set this GPU treats `dim_x` as a thread count, not a group
  count: 3907 "groups" of 256 wrote exactly 3907 elements. kfdtest never
  notices because it dispatches 1 thread per group. pm4bridge uses `0x1`
  (the Garlic value).
- **`PGM_RSRC1/2`**: taken from the kernel descriptor. kfdtest hard-codes
  them and sets `TRAP_PRESENT` and `EXCP_EN_MSB`; clang's descriptor sets
  neither and the kernels run correctly.
- **ATC**: KFD reports this GPU as a dGPU-style node (no CPU cores on the
  node), so `PGM_HI` and the dispatch initiator carry no ATC bit, memory must
  be mapped explicitly (`hsaKmtMapMemoryToGPUNodes`) and CPU and GPU addresses
  are identical. The buffer resource word clang emits is `0x100f000`.
- **CU masks**: pm4bridge does not write `STATIC_THREAD_MGMT_SE0..3` by
  default (kfdtest does not either; KFD programs them from the queue's MQD).
  Writing them (`PM4_CU_MASK`) made no measurable difference, whether with all
  CUs or with one CU per shader engine. Whether the write is honoured was not
  checked.
- **Partial flush**: kfdtest's `RELEASE_MEM` + `WAIT_REG_MEM` pair is used in
  place of `EVENT_WRITE CS_PARTIAL_FLUSH`. The plain partial flush was not tried.
- **Topology**: KFD reports 4 arrays x 10 CUs (144 SIMDs), not 4 x 9.
- `buffer_load_dwordx4` / `buffer_store_dwordx4` return correct data (the
  b128 problem is LDS only).

## Known limits

- gfx7 only, one GPU node, one PM4 queue per context, not thread-safe.
- 1-D grids only; `block_x` <= 256; kernarg <= 512 bytes; 64 dispatches in
  flight (a 65th `pm4_dispatch` waits for the oldest, bounded by `PM4_TIMEOUT_MS`).
- No scratch, no `get_global_id`/`get_global_size`, no global variables or
  relocations, no function calls.
- `pm4_wait` polls a fence: it spins for about 2 ms, then sleeps 50 us between polls.
- **The timeout path has never met a real hang.** No dispatch hung during
  development, so `PM4_ERR_TIMEOUT` is only covered by code review. After a
  timeout the context refuses new work (`PM4_ERR_WEDGED`); `pm4_shutdown` will
  still destroy the queue, which makes KFD try to preempt it.
- Dynamic LDS sizing is implemented (`group_segment_fixed_size` -> `RSRC2`)
  but no test uses LDS.
- On cached memory write bandwidth is about 40 % of read bandwidth; on write-combined memory it is about 65 %. The reason is not known.

## Safety

`run.sh <test> <timeout> [args]` counts kernel trouble lines
(`ring .*timeout|preemption time out|Failed to evict|kfd.*(hang|reset)`)
before and after each test, prints any new kernel log lines, and flags
leftover or stuck processes. During development the count stayed at 0 and no
test hung.

## Files

    pm4bridge.h / pm4bridge.c   the library
    kernels/*.cl                device code; kernels/audit.sh enforces the hardware rules
    kernels/bad/scratch.cl      negative test input (needs scratch; never dispatched)
    tests/t_*.c, tests/common.h tests
    run.sh                      test runner with timeout and kernel-log check
    logs/                       logs of the last `make check`

The PM4 packet layouts and the dispatch sequence are adapted from the ROCm
kfdtest sources (MIT license, Copyright (C) 2014-2018 Advanced Micro Devices,
Inc.); see the notice at the top of `pm4bridge.c`.
