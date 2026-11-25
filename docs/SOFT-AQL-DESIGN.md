# Soft-AQL: AQL → PM4 translation inside ROCr 6.4.4 (PS4 Pro / Gladius)

Status: **design only, nothing built or run.** Written 2026-11-25 from source reading:
- ROCr/libhsakmt/kfdtest `rocm-6.4.4`
- CLR `rocm-6.2.4` (sparse checkout of `rocclr/device`)
- `rocm/rocr-6.4.4-ps4-gfx7.patch` and `docs/FINDINGS.md` §2c.

File:line references are to those trees. Statements marked **(unverified)** are inferences that have not been tested on the Pro.

## 0. Problem in one paragraph

On this firmware (MEC fw 421), AQL queue creation through KFD works, but the MEC never acts as an AQL packet processor:
- it executes PM4 placed in a slot,
- but it never advances `read_dispatch_id` and never invalidates the slot (`AqlQueue::ExecutePM4`, `amd_aql_queue.cpp:1546`, gfx≤8 branch, `RELEASE_MEM event_index 7`).

PM4 compute queues do work. kfdtest proved `DISPATCH_DIRECT` (inside an IB), `RELEASE_MEM` → EOP event, `WRITE_DATA` and `WAIT_REG_MEM` on this box.

Soft-AQL keeps the HSA contract unchanged: applications still get an `hsa_queue_t` AQL ring plus a doorbell signal. ROCr consumes that ring on the CPU and replays it as PM4 on a KFD `HSA_QUEUE_COMPUTE` queue.

## 1. Hook point: a new `AMD::SoftAqlQueue`, selected in `GpuAgent::QueueCreate`

**Choice:** add a new `core::Queue` implementation. `GpuAgent::QueueCreate` (`amd_gpu_agent.cpp:1667`; the `new AqlQueue(...)` is at `:1742`) instantiates it in place of `AqlQueue` when soft-AQL is active:
- ISA major == 7, **and**
- `HSA_SOFT_AQL=1`, or PCI device id ∈ {0x9920, 0x9924} with no `HSA_SOFT_AQL=0` override. The flag goes in `core/util/flag.h`.

Every queue ROCr hands out goes through this function:
- `hsa_queue_create` → `agent->QueueCreate` (`hsa.cpp:746`)
- internal utility/blit queues: `GpuAgent::CreateInterceptibleQueue` (`amd_gpu_agent.cpp:741`) → `QueueCreate`
- `hsa_amd_queue_intercept_create` wraps an `hsa_queue_create` queue (`hsa_ext_amd.cpp:1088`)

So one switch covers apps, ROCr internals and tools.

Why not the alternatives:

| Option | Verdict |
|---|---|
| Subclass `AqlQueue` | No. Its constructor (`amd_aql_queue.cpp:79-360`) is built around a KFD **AQL** queue: `hsaKmtCreateQueueExt(HSA_QUEUE_COMPUTE_AQL)`, legacy doorbell proxy, scratch SRD, `queue_inactive_signal`/exception handlers and `pm4_ib_buf_`. All of that would have to be bypassed, and every override would fight the base class. |
| Extend `InterceptQueue` | No, but **copy its structure**. It needs a *wrapped* hardware AQL queue (`InterceptQueue::Submit`, `intercept_queue.cpp:206`, writes into `wrapped->...base_address` and rings `wrapped`'s doorbell), and that queue never advances here. Its consumer logic is exactly the right template, though (next row). |
| New `SoftAqlQueue : core::Queue, private core::LocalSignal, public core::DoorbellSignal` | **Yes.** Same shape as `InterceptQueue` (`intercept_queue.h`, `QueueProxy` index ops). The host doorbell is a virtual `StoreRelaxed` call (`hsa_signal_store_*` → `signal->StoreRelease`, `hsa.cpp:1210-1225`). CLR rings it exactly that way (`rocvirtual.cpp:928/1075/1153`), as does ROCr's `BlitKernel` (`amd_blit_kernel.cpp:1237`). So translation can run **synchronously in the producer's thread**, like `InterceptQueue::StoreRelaxed` (`intercept_queue.cpp:320`). |

New files (phase 1 stays within 5 files):
- `core/inc/amd_soft_aql_queue.h`
- `core/runtime/amd_soft_aql_queue.cpp`
- `core/runtime/amd_blit_cpu.{h,cpp}` (§5)
- edits to `amd_gpu_agent.cpp` (QueueCreate, InitDma) and `flag.h`.

Things that must also change:
- **Do not** push soft queues into `aql_queues_` (`amd_gpu_agent.cpp:1744`). `AsyncReclaimScratchQueues` (`:2042`) `static_cast`s every entry to `AqlQueue*`.
- `InitGWS` casts too (`:941`), but it only runs when `NumGws != 0`, which is not the case on gfx7.
- `InterceptQueue::GetInfo` (`intercept_queue.cpp:403`) returns `INVALID_QUEUE` for a non-`AqlQueue` wrapped queue. That only affects tools. Optionally add `SoftAqlQueue::IsType` there.

### SoftAqlQueue state

- **AQL ring**: `size` × 64 B in system memory, headers initialised to INVALID (as in `AqlQueue`). `amd_queue_` lives in the `Queue` base `SharedQueue` (`queue.h:181`):
  - fill `hsa_queue` (type MULTI, `features = KERNEL_DISPATCH`, `base_address`, `doorbell_signal = Signal::Convert(this)`, `size`, `id = GetQueueId()`);
  - fill the group/private aperture `_hi` fields from the agent's regions (copy the loop in `amd_aql_queue.cpp:166-190`);
  - fill `max_cu_id` and `max_wave_id`.
  This is what the kernel's `queue_ptr` points at.
- **Doorbell signal block**: kind `AMD_SIGNAL_KIND_USER` with no hardware pointer. Host stores dispatch through the vtable. There is no GPU-side enqueue on gfx7.
- **PM4 queue**: `hsaKmtCreateQueue(node, HSA_QUEUE_COMPUTE, 100, NORMAL, ring, ring_bytes, NULL, &rsrc)`. This is kfdtest's `BaseQueue::Create` path; the `queues.c:674` `KFD_IOC_QUEUE_TYPE_COMPUTE` branch supplies the rptr/wptr. Allocate the ring the way kfdtest does: host memory, GPU-mapped, exec, zeroed (`BaseQueue.cpp:51`). Submit like `PM4Queue::SubmitPacket` (`PM4Queue.cpp:57`), the pre-gfx9 path:
  ```
  *rsrc.Queue_write_ptr = wptr_dw;  mb();  *rsrc.Queue_DoorBell = wptr_dw;   // dwords, wraps at ring size
  ```
  Pad with single-dword NOPs (`0xFFFF1002`, kfdtest `CMD_NOP_TYPE_3`, `BaseQueue.hpp:94`) instead of letting a packet cross the ring end (`BaseQueue::PlacePacket`, `BaseQueue.cpp:142-163`).
- **Slot arena**: `M` slots, with `M = min(size, 1024)` in-flight packets. Each slot is 1 KiB, **allocated uncached** (kfdtest `HsaMemoryBuffer(..., isUncached=true)` equivalent: `hsaKmtAllocMemory` with `Uncached=1`, then map). Layout:
  - `[0..63]`: shadow copy of the AQL packet. This is the `dispatch_ptr` target, so the producer may reuse the ring slot immediately.
  - `[64..1023]`: the slot's PM4 IB, up to 240 dw.
  Uncached matters: a slot gets reused, and the shader reads its `dispatch_ptr` through K$/L2. Garlic: *"the L2 does not see CPU writes without an invalidate."*
- **Fence**: one 64-bit `fence` per queue in uncached memory. RELEASE_MEM writes `aql_index + 1` there at EOP.
- **Indices**: `next_packet_` (translated), `retired_` (fence-confirmed), and `amd_queue_.read_dispatch_id`. The read index is advanced at **translation** time, because the packet has been copied to the shadow.

## 2. Packet translation

The PM4 type-3 header is `PM4_HDR(op, ndw, 7)` (`amd_gpu_pm4.h:65`): `(3<<30) | ((ndw-2)<<16) | (op<<8) | (1<<1)`. Shader-type 1 marks the packet as compute. Opcodes (kfdtest `gfx_7_2_enum.h`):

| packet | value |
|---|---|
| NOP | 0x10 |
| DISPATCH_DIRECT | 0x15 |
| WRITE_DATA | 0x37 |
| WAIT_REG_MEM | 0x3C |
| INDIRECT_BUFFER | 0x3F |
| EVENT_WRITE | 0x46 |
| RELEASE_MEM | 0x49 |
| ACQUIRE_MEM | 0x58 |
| SET_SH_REG | 0x76 |

SET_SH_REG offsets are `reg - 0x2C00` (`PERSISTENT_SPACE_START`). gfx7 registers (`gfx_7_2_d.h`):

| register | address | SET_SH_REG offset |
|---|---|---|
| COMPUTE_START_X | 0x2E04 | 0x204 |
| COMPUTE_PGM_LO | 0x2E0C | 0x20C |
| COMPUTE_PGM_RSRC1 | 0x2E12 | 0x212 |
| COMPUTE_RESOURCE_LIMITS | 0x2E15 | 0x215 |
| COMPUTE_TMPRING_SIZE | 0x2E18 | 0x218 |
| COMPUTE_RESTART_X | 0x2E1B | 0x21B |
| COMPUTE_USER_DATA_0 | 0x2E40 | 0x240 |

**Layout per AQL packet.** Everything packet-specific goes into the slot's IB. The ring gets a fixed 18 dw per packet, so ring space is bounded (`M × 18 × 4` B plus padding; 128 KiB is enough for M=1024):

```
ring:  INDIRECT_BUFFER(slot IB)             4 dw   (kfdtest PM4IndirectBufPacket, PM4Packet.cpp:244)
       RELEASE_MEM  fence ← aql_index+1     7 dw   (no interrupt)
       RELEASE_MEM  mailbox ← event_id      7 dw   (interrupt; only if the packet has a completion signal or is a kick point, otherwise NOP 7)
```

This is exactly kfdtest's proven shape (`Dispatch::Submit`, `Dispatch.cpp:81-99`): an IB, then a mailbox write with interrupt.

### 2a. KERNEL_DISPATCH

Validate first (reject → §2e error path):

1. Read the descriptor at `kernel_object`. The CP-relevant offsets are identical in `amd_kernel_code_t` (v1; ROCr's own blit shaders, `AssembleShader`, `amd_gpu_agent.cpp:430-466`) and in `kernel_descriptor_t` (v3+, `loader/AMDHSAKernelDescriptor.h`):

   | offset | field |
   |---|---|
   | +0 | group_segment_fixed_size (v3+) |
   | +4 | private_segment_fixed_size (v3+) |
   | +16 | kernel_code_entry_byte_offset (s64) |
   | +48 | compute_pgm_rsrc1 |
   | +52 | compute_pgm_rsrc2 |
   | +56 | kernel_code_properties |

2. Reject the packet if any of these hold:
   - `rsrc2.SCRATCH_EN` (bit 0)
   - `private_segment_fixed_size` > 0 (v3+)
   - `packet.private_segment_size` > 0
   - `USES_DYNAMIC_STACK` (prop bit 11)
   - `ENABLE_WAVEFRONT_SIZE32` (bit 10)
   - `code = kernel_object + entry_offset` not 256-B aligned
3. Build the user-SGPR list (§2b). Require `count == rsrc2.USER_SGPR` (bits 5:1) and `count ≤ 16`; otherwise reject.
4. Force `rsrc2.TRAP_PRESENT` (bit 6) = 0. There is no gfx7 trap handler (`BindTrapHandler` returns early, `amd_gpu_agent.cpp:2243`).
5. Set `rsrc2.LDS_SIZE` (bits 23:15) = `ceil(packet.group_segment_size / 512)`. CIK's granularity is 128 dw. COv3+ leaves the field 0 for the CP to fill. **(unverified: 512 B granularity on Gladius, same as Hawaii.)** Reject if > 64 KiB.

**Slot IB** (all values literal; `n` = user SGPR count):

```
# optional: packet.header.BARRIER==1 and earlier packets still in flight → drain (kfdtest Dispatch.cpp:269-271 pattern)
RELEASE_MEM   C0054902 04028514 43000000 lo(drain) hi(drain) tok 0     # EOP, l2_wb+l2_inv, int_sel=3 (no IRQ), data_sel=2 (64b)
WAIT_REG_MEM  C0053C02 00000013 lo(drain) hi(drain) tok FFFFFFFF 80000004  # mem space, func "==", poll 4

# acquire (always on gfx7: "L1 not coherent between dispatches")
ACQUIRE_MEM   C0055802 coher FFFFFFFF 000000FF 0 0 00000004
              coher = 0x08400000 (SH_KCACHE|TCL1)            acquire scope NONE/AGENT
                    | 0x00800000 (TC = L2 inv)               acquire scope SYSTEM
              (kfdtest uses 0x28C00000 = +ICACHE; ROCr's InvalidateCodeCaches adds TC_WB bit 18)

SET_SH_REG    C0087602 00000204  0 0 0  wg_x wg_y wg_z  0 0     # START_XYZ, NUM_THREAD_XYZ, PIPESTAT, PERFCNT
SET_SH_REG    C0027602 0000020C  code>>8  (code>>40)&0xFF [|1<<8 ATC iff !dgpu]
SET_SH_REG    C0027602 00000212  rsrc1  rsrc2'
SET_SH_REG    C0017602 00000215  0                                   # RESOURCE_LIMITS
SET_SH_REG    C0017602 00000218  0                                   # TMPRING_SIZE (no scratch)
SET_SH_REG    C0047602 0000021B  0 0 0 0                             # RESTART_XYZ, THREAD_TRACE_ENABLE
SET_SH_REG    C0007602|(n<<16) 00000240  u0 … u(n-1)                 # USER_DATA, §2b
DISPATCH_DIRECT C0031502  grid_x grid_y grid_z  00000021 [|0x1000 DATA_ATC iff !dgpu]
```

Notes:
- `DISPATCH_INITIATOR = 0x21` (COMPUTE_SHADER_EN | USE_THREAD_DIMENSIONS) is kfdtest's proven value (`Dispatch.cpp:225`).
  - With USE_THREAD_DIMENSIONS the DIM_* fields are **threads**, so AQL `grid_size_*` maps 1:1 and `NUM_THREAD_*` = `workgroup_size_*`.
  - kfdtest only ever used grid % wg == 0, so **partial last workgroups are unverified**. Fallback: dims = `ceil(grid/wg)` groups without USE_THREAD_DIMENSIONS, plus `PARTIAL_TG_EN` (bit 1) and `NUM_THREAD_PARTIAL` (bits 31:16). That is RADV's gfx7 path.
- The ATC/DATA_ATC bits follow `hsakmt_is_dgpu()` in kfdtest (`Dispatch.cpp:172, 225`). The KFD patch takes the Hawaii dGPU path with ATC off, so **expect no ATC bits**. Log the agent's dGPU-ness at queue creation and keep it consistent with what kfdtest ran.
- Emit every SH register for every dispatch. Do not assume the HQD preserves COMPUTE_* across queues on the same pipe.
- Release (ring part, after the IB):
  - fence `RELEASE_MEM C0054902 04028514 43000000 lo hi seq_lo seq_hi` for SYSTEM release scope (`l2_wb` bit 15 + `l2_inv` bit 17, as kfdtest `InitPacketCI` `PM4Packet.cpp:83-146`);
  - `dw1 = 00000514` (no cache action) for AGENT/NONE release.
  - An EOP fence also implies every earlier packet on this queue has finished, so retirement is in order.

### 2b. User SGPR layout (from `kernel_code_properties`, AMDGPU ABI order)

| bit | SGPRs | value written to USER_DATA |
|---|---|---|
| 0 ENABLE_SGPR_PRIVATE_SEGMENT_BUFFER | 4 | **null scratch SRD**: `{0,0,0,0x100F000}` (dst-sel/format word from the kfdtest no-FLAT shader; base 0, num_records 0 ⇒ out-of-range stores dropped). Clang sets this bit on essentially every gfx7 HSA kernel, so it must be supplied even without scratch. |
| 1 DISPATCH_PTR | 2 | address of the **slot shadow packet** (64 B copy, `header` already valid) |
| 2 QUEUE_PTR | 2 | `&amd_queue_` (apertures used by pre-gfx9 address-space casts) |
| 3 KERNARG_SEGMENT_PTR | 2 | `packet.kernarg_address` |
| 4 DISPATCH_ID | 2 | AQL packet index |
| 5 FLAT_SCRATCH_INIT | 2 | `0, 0` |
| 6 PRIVATE_SEGMENT_SIZE | 1 | `packet.private_segment_size` (0) |
| 7-9 GRID_WORKGROUP_COUNT_X/Y/Z (**v1 `amd_kernel_code_t` only**; reserved in v3+) | 1 each | `ceil(grid_x/wg_x)`, … |

The system SGPRs (workgroup id X/Y/Z, workgroup info, wave offset) are produced by hardware from the descriptor's `rsrc2` TGID_*/TG_SIZE_EN bits. Nothing to synthesise there.

COv5 hidden kernargs (`hidden_block_count`, `group_size`, `remainder`, `private_base`/`shared_base`, …) are the producer's job (app or CLR), not the packet processor's. They pass through untouched.

### 2c. BARRIER_AND / BARRIER_OR

- **BARRIER_AND**: for each non-null `dep_signal[i]` (≤5), emit two waits into the slot IB on `amd_signal_t.value`, which sits at `handle+8`:
  ```
  WAIT_REG_MEM C0053C02 00000013 lo(h+8)  hi(h+8)  0 FFFFFFFF 80000004   # value.lo == 0
  WAIT_REG_MEM C0053C02 00000013 lo(h+12) hi(h+12) 0 FFFFFFFF 80000004   # value.hi == 0
  ```
  - The barrier bit gives the drain prologue from §2a; the acquire/release scopes give ACQUIRE_MEM/RELEASE_MEM as for a dispatch; the completion signal works as in §3.
  - Deadlock check: the CPU retire thread (§3) decrements dep signals completed on *any* soft queue, independently of the stalled MEC queue, so the GPU wait always ends unless the app made a genuine cycle.
- **BARRIER_OR**: `WAIT_REG_MEM` cannot OR across addresses, so do this on the CPU.
  - The translator **parks** at this packet: stop, do not advance the read index, register `Runtime::SetAsyncSignalHandler` (`runtime.h:327`) on each dep signal with `CONDITION_EQ 0`.
  - The first handler to fire unparks the queue and re-runs the translator, which then emits a no-op slot (fence only).
  - The producer thread never blocks.
- **AMD vendor `BARRIER_VALUE`** (`hsa_amd_barrier_value_packet_t`, `hsa_ext_amd.h`). CLR emits these routinely (`dispatchBarrierValuePacket`, `rocvirtual.cpp:1102`; enabled for every device, `rocdevice.cpp:1757`).
  - If `mask >> 32 == 0` or the high dword is pinned: emit a single `WAIT_REG_MEM` on the low dword, `ref = value`, `mask = mask`, with func ∈ {EQ=3, NE=4, LT=1, GTE=5}. Values are unsigned 32-bit.
  - CLR's common case (`cond LT, value 1, mask INT64_MAX`): `hi&0x7FFFFFFF == 0`, then `lo < 1`.
  - Anything else: CPU park, as for BARRIER_OR.

### 2d. VENDOR_SPECIFIC / ROCr's PM4

- `amd_vendor.format == 1` (`AMD_AQL_FORMAT_PM4_IB`, the gfx9+ `ExecutePM4` format, `amd_aql_queue.cpp:1628-1652`): copy `ib_jump_cmd[4]` (an INDIRECT_BUFFER) into the slot IB and complete as usual. That costs little, and tools/CLR perf-counter code may use it.
- `AMD_AQL_FORMAT_INTERCEPT_MARKER` (0xFE): invoke the callback as `InterceptQueue::Submit` does, then treat the packet as a NOP.
- gfx≤8 "raw PM4 in the slot" (`ExecutePM4`'s own trick): never produced, because `ExecutePM4` is overridden (§6).

### 2e. Errors

An unknown packet type or a rejected dispatch is handled like the CP does:
- set the queue inactive;
- call the queue's `HsaEventCallback` with `HSA_STATUS_ERROR_INVALID_PACKET_FORMAT`, or `HSA_STATUS_ERROR_INVALID_ISA` for a rejected descriptor, with a message naming the reason (`scratch`, `wave32`, `user_sgpr mismatch`, `FLAT found`, …);
- do not advance past the packet.

## 3. Signals and completion: CPU-side retire, woken by the EOP interrupt

**MVP: CPU completes the signals.**

- **One KFD event per agent**: `hsaKmtCreateEvent(HSA_EVENTTYPE_SIGNAL, …)`, or `core::InterruptSignal::CreateEvent`. Its mailbox `EventData.HWData2` and `EventId` are what the ring's second RELEASE_MEM writes, with `int_sel=2` (interrupt after write confirm). That is **exactly** kfdtest's EOP path, which delivered events on this KFD:
  ```
  RELEASE_MEM C0054902 04028514 22000000 lo(HWData2) hi(HWData2) EventId 0
  ```
- **One retire thread per agent**:
  ```
  for (;;) {
    hsaKmtWaitOnEvent(evt, wd_ms)            // or spin HSA_SOFT_AQL_SPIN_US first
    for each soft queue q:
      f = load_acquire(q->fence)
      while (q->retired_ < f) {
        s = q->slot[q->retired_ % M].shadow.completion_signal
        if (s.handle) core::Signal::Convert(s)->SubRelease(1)   // HSA semantics: atomic decrement
        q->retired_++
      }
      if (q->parked_for_slots) q->Kick()     // re-run translator outside the retire loop
  }
  ```
  `SubRelease` is virtual, so the decrement does the right thing for each signal type:
  - `DefaultSignal`: plain decrement.
  - `InterruptSignal`: decrement, then `hsaKmtSetEvent` if a waiter is blocked.
  - IPC signals and CLR's `HSA_AMD_SIGNAL_AMD_GPU_ONLY` signals (`rocmemory.cpp:833`) work unchanged.
  - Waiters of `hsa_signal_wait_*` need no changes.

Why not have the GPU write signals directly:
- HSA completion is an *atomic decrement* by 1, and a RELEASE_MEM can only *write* a value. The queue does not know the signal's current value (CLR shares signals; ROCr gang signals).
- The decrement would have to be done by an `ATOMIC_MEM` packet after an EOP wait, followed by the mailbox interrupt. The MEC fw 421 support for ATOMIC_MEM is **unverified**.
- That variant is a **phase 2 latency optimisation** for signals that are not IPC. Keep the CPU path as the reference.

**Watchdog:** if `fence` hasn't moved for `HSA_SOFT_AQL_TIMEOUT_MS` while packets are in flight:
- log the queue, the last translated packet and the descriptor;
- set the queue inactive and fire the error callback.
- **Do not** destroy the KFD queue automatically: queue destroy with live waves is what produced the preemption timeout and the D-state hang in §2c. Leave that to process exit.

## 4. The consumer: no polling thread on the submit side

- **Submit side.** Host producers end with `hsa_signal_store_*(doorbell, idx)`, which calls `SoftAqlQueue::StoreRelaxed(idx)` virtually. That function:
  - takes `lock_`,
  - scans from `next_packet_` while `header != INVALID` (acquire load), stopping early when `next_packet_ - retired_ == M` (park; the retire thread kicks),
  - copies each packet to its slot shadow and builds the slot IB,
  - writes the 18 ring dwords,
  - sets the ring header to INVALID and advances `read_dispatch_id` (release),
  - writes wptr plus the doorbell **once per batch**.

  This mirrors `InterceptQueue::StoreRelaxed` (`intercept_queue.cpp:320-395`).
- **Re-entrancy.** A doorbell store made *inside* the translator (an intercept-marker callback) is deferred to the retire thread's kick, as `InterceptQueue` does with `Cursor`.
- **Device-side enqueue** (GPU writes the doorbell): unsupported. Nothing on gfx7 HIP/HSA uses it.
- **Completion side.** One retire thread per agent blocks on the KFD event, so there is no futex on the AQL ring. Optionally spin on `fence` for `HSA_SOFT_AQL_SPIN_US` before blocking, for low-latency mode (it costs a Jaguar core while spinning).

**Latency estimate (unmeasured, Jaguar @ 2.1 GHz):**

| stage | cost |
|---|---|
| Translate + IB build (~75 dw) + 18 ring dw + 1 MMIO doorbell (producer thread) | **~2–4 µs per dispatch**, amortised across a batch |
| CP cost per dispatch (ACQUIRE_MEM + ~40 dw of SET_SH_REG + DISPATCH_DIRECT) | **~2–5 µs**. Similar to what the AQL firmware does internally, plus the IB fetch. |
| Completion seen by a blocked `hsa_signal_wait`: EOP IRQ → KFD IH/workqueue → retire thread wake → `SubRelease` → `SetEvent` → waiter wake (two wakeups) | **~25–60 µs per synchronous round trip**, vs ~10–20 µs for hardware AQL. With spin mode, **~3–8 µs**. |

Pipelined throughput (llama.cpp-style queues of hundreds of dispatches per token) is limited by the producer-side ~3–6 µs/dispatch, not by completion latency. Per-token sync then adds one round trip. Spin mode matters for `tg` (FINDINGS: generation may be submit-latency bound).

## 5. Blits: CPU memcpy (recommended)

| Option | Assessment |
|---|---|
| **CPU `memcpy`/`memset`** | **Recommended.** The Pro is a unified-memory APU. KFD exposes a single public 3.9 GB GTT bank, `large_bar_enabled_` is set (the loader already takes the `memcpy` path for small segments, `amd_loader_context.cpp:359-366`), and every agent allocation is CPU-mapped. Zero GPU risk, no FLAT, no queues. Coherence is handled by the dispatch-side ACQUIRE_MEM (system scope → L2 inv) and the RELEASE_MEM `l2_wb`. |
| SDMA (`BlitSdmaV2V3`, chosen for gfx7 at `amd_gpu_agent.cpp:763`) | Phase 3 at most. Untested on Gladius, so it is another engine and firmware to bring up. Note: stock `InitDma` **would select SDMA for H2D/D2H on gfx7** (only gfx8 is excluded, `:840`), so it must be overridden explicitly. |
| MUBUF rewrite of `blit_copyAligned.s` / `blit_fill.s` | Feasible: they are small. Replace the `FLAT_LOAD_*`/`FLAT_STORE_*` macros with `buffer_load/store_* v[a:a+1], s[srd], 0 addr64` and `SRD {0,0,0,0x100F000}`, the same trick as the kfdtest `CopyDwordIsa` patch. Then regenerate `kCodeCopyAligned7`/`kCodeFill7` in `amd_blit_shaders.h`, or assemble gfx7 in `blit_shaders/CMakeLists.txt`. Only worth it if CPU copy becomes a bandwidth problem. Jaguar memcpy is probably ~5–10 GB/s against ~218 GB/s of GDDR5 for GPU copies. |

Implementation: `AMD::BlitCpu : core::Blit` in `amd_blit_cpu.{h,cpp}`.
- Interface (`core/inc/blit.h:55-121`):
  - sync `SubmitLinearCopyCommand`: `memcpy`.
  - async `SubmitLinearCopyCommand(dst, src, size, deps, out, gang)`: enqueue to a single worker thread. The worker CPU-waits each dep signal (`WaitAcquire(EQ? LT 1)`), copies, `_mm_sfence`, then `out.SubRelease(1)`. Async is required because a dep may be satisfied by work submitted later from the same thread.
  - `SubmitLinearFillCommand`: 32-bit fill.
  - `isSDMA()` returns false; `PendingBytes` returns the queued bytes; `GangLeader`: no-op.
- In `GpuAgent::InitDma` (`amd_gpu_agent.cpp:813`), when soft-AQL is on, `blits_[BlitDevToDev/HostToDev/DevToHost]` all create `BlitCpu`.
- This also fixes the **latent** problem in `hsa_executable_freeze`: `RegionMemory::Freeze` calls `agent->DmaCopy` → `blits_[BlitDevToDev]` → `BlitKernel`, which would run the FLAT kernel. That happens once a code object exceeds `co_dmacopy_size` (`amd_loader_context.cpp:359-362`, `GpuAgent::DmaCopy` `:972`).
- `DmaCopyRect` already refuses gfx<9 (`:1263`); callers fall back.

## 6. `InvalidateCodeCaches` and other ROCr-emitted PM4

`ExecutePM4` is virtual on `core::Queue` (`queue.h:364`). `SoftAqlQueue::ExecutePM4(cmd, size, acq, rel, signal)` does the following:
1. Take a slot under `lock_` (waiting for a free slot, **without** holding the lock while it waits).
2. `memcpy` the command into the slot IB. ROCr's limit is 0x1000 B; size the IB or chain a second buffer for the full 1024 dw.
3. Emit the standard 18-dw ring tail with a mailbox interrupt.
4. Release the lock and wait until `retired_ > that index`.
5. If `signal` is set, store 0 (gfx≤8 semantics, `amd_aql_queue.cpp:1677`).

It needs no AQL slot and makes no read-index assumption. With fw 421 ≥ 420, the gfx7 branch of `GpuAgent::InvalidateCodeCaches` (`amd_gpu_agent.cpp:2284-2340`) runs and builds `ACQUIRE_MEM` with SH_ICACHE|SH_KCACHE|TC|TC_WB over the code range. That is a valid gfx7 MEC packet: kfdtest's `0x28C00000` variant already ran, and so did the `PS4_ICACHE_MODE=kfdtest` probe in our patch. Once this exists, **delete** the `PS4_ICACHE_MODE` and `PS4_PM4_TIMEOUT_MS` bring-up hacks from `rocr-6.4.4-ps4-gfx7.patch`.

The other `ExecutePM4` callers are PC sampling only (`:3041`, `:3141`), which is gfx9+, so they are not reached.

## 7. Scratch / private segment: reject

Reject at dispatch (§2a). Also add a cheap **load-time warning**: walk the kernel symbols after `hsa_executable_freeze` and log any kernel with `private_segment_size > 0` or a dynamic stack, so the failure shows up early instead of at first launch.

Reasons:
- Garlic reports that scratch is unsupported.
- `cwsr_size 0`, and the scratch path is untested.
- A wrong `TMPRING`/SRD turns a bad kernel into a GPU hang, and a hang becomes the KFD wedge.

If it is ever needed, the encodings already exist:
- `Dispatch::SetScratch` (`Dispatch.cpp:67`): `TMPRING_SIZE = WAVESIZE<<12 | WAVES`, user SGPRs 0–3 = SRD, plus `SCRATCH_EN`.
- `AqlQueue::FillComputeTmpRingSize` / `InitScratchSRD` (`amd_aql_queue.cpp:1823, 1957`).
- `GpuAgent::AcquireQueueMainScratch` for the allocation.

The build already prevents most scratch use: `-O2`/`-O3`, no spills, which needs a check.

## 8. HIP / CLR 6.2.4: what it additionally needs (verdict: feasible later; scope phase 1 to HSA-only)

What already lines up:
- CLR 6.2.4 lists `gfx701` "Hawaii" as runtime-supported (`device.cpp:116`).
- It rings doorbells via `hsa_signal_store_screlease`, which works with §4.

What it needs beyond the MVP:
1. **`BARRIER_AND` + AMD `BARRIER_VALUE`** (§2c). CLR uses both on every stream sync (`rocvirtual.cpp:1029, 1102, 3460`).
2. **`hsa_amd_memory_async_copy[_on_engine]`** (`rocblit.cpp`) → `BlitCpu` (§5).
3. **Profiling timestamps** (hipEvent elapsed time; `hsa_amd_profiling_set_profiler_enabled`, `rocdevice.cpp`; `get_dispatch_time`, `rocvirtual.cpp`):
   - before the dispatch: RELEASE_MEM with `data_sel=3` (64-bit GPU clock) to `&signal->start_ts`;
   - after it: the same to `&signal->end_ts`;
   - set `SetProfiling` from `queue_properties`. `GpuAgent::TranslateTime` already converts ticks.
4. **`hsa_amd_queue_cu_set_mask`** → `SetCUMasking`: `hsaKmtSetQueueCUMask` on the PM4 queue, or accept and ignore.
5. **CLR's own blit kernels** are OpenCL compiled through comgr at runtime (`device.cpp:536`, `BlitProgram::create`). They must get `-Xclang -target-feature -Xclang -flat-for-global` and `-enable-ds128` disabled through `extraOptions` or a comgr patch. The same applies to **all** HIP device code (hipcc flags).
6. **Biggest HIP risk: generic pointers.** `-flat-for-global` only lowers *addrspace(1)* accesses to MUBUF. Any generic (`addrspace 0`) access that InferAddressSpaces cannot resolve must still be `flat_load/store`, and that hangs this GPU. HIP code is generic-pointer heavy, and so are the ockl/ocml device libs. A build-time gate is mandatory: `llvm-objdump -d | grep -E '\bflat_'` must be empty. Optionally add a load-time ISA scan in ROCr: decode the SOP/VOP/SMRD/MUBUF/FLAT encodings, where FLAT is `bits[31:26]=0b110111`, and refuse the code object.
7. **Out of scope:** printf/hostcall, device malloc and asserts (no trap handler, so `s_trap` is ignored), cooperative groups/GWS (NumGws 0), and `hipMemcpy2D` through the SDMA rect path (gfx<9 refuses; CLR falls back to its blit kernels, see item 5).

## 9. Effort, risks, minimal viable slice

### MVP slice: "freeze + one kernel"
1. `SoftAqlQueue`: ring, indices, doorbell→translator, PM4 queue create/destroy, `KERNEL_DISPATCH` only (BARRIER_AND with **zero** deps = fence-only NOP), completion via retire thread, `ExecutePM4` override.
2. `BlitCpu` for all three blit slots.
3. Selection in `QueueCreate` / `InitDma` behind `HSA_SOFT_AQL`.
4. Test ladder, each step on its own and with a fresh boot available:
   1. `hsa_init` + `rocminfo`
   2. `hsa_executable_freeze` (exercises `InvalidateCodeCaches` → `ExecutePM4`)
   3. a no-FLAT `-mcpu=hawaii` copy kernel with grid % wg == 0
   4. the same with grid % wg ≠ 0 (partial WG)
   5. 10⁴ back-to-back dispatches + one wait (throughput)
   6. `hsa_signal_wait` latency, interrupt vs spin.

### Effort (one engineer who knows ROCr; hardware debug is the unknown)

| component | build | hardware bring-up | main risk |
|---|---|---|---|
| SoftAqlQueue skeleton (Queue iface, ring, PM4 queue, doorbell, slots) | 2–3 d | 1–2 d | KFD PM4 queue sizing; NOP wrap |
| KERNEL_DISPATCH translation + descriptor validation + USER_DATA | 2 d | 2–4 d | USE_THREAD_DIMENSIONS partial WG; LDS granularity; ATC bits |
| Retire thread + EOP event + signals | 2 d | 1–3 d | IRQ→event delivery (PS4 IH "no PASID" quirk seen once); spin fallback |
| ExecutePM4 override | 0.5 d | 0.5 d | low |
| BlitCpu | 1–2 d | 0.5 d | coherence (L2 inv on SYSTEM acquire) |
| **MVP total** | **~8–10 d** | **~5–10 d** | **≈ 2.5–4 weeks wall time** |
| Barriers AND/OR + BARRIER_VALUE, error callbacks, watchdog | 3 d | 2 d | GPU wait on CPU-retired signals (should be fine) |
| Profiling timestamps | 1–2 d | 1 d | low |
| HIP/CLR 6.2.4 (build, compiler flags, comgr, FLAT gate, tests) | 1–2 wk | 1–3 wk | generic-pointer FLAT; CLR assumptions |

### Biggest risks (ordered)
1. **A FLAT instruction in any kernel hangs the GPU, and a hang wedges KFD** (preemption timeout → D-state → reboot). Soft-AQL cannot catch a hang. Its only defences are the ISA gate/scan and the descriptor checks. A test kernel that slips through costs a reboot.
2. **EOP-interrupt → KFD event delivery** under sustained load on this PS4 IH path. It was proven once by kfdtest, and the "FW bug: No PASID" WARN was seen once and not since. Fallback: retire-thread polling with a 1 ms timed wait still completes correctly, just slower.
3. **Cache coherence** of kernargs and the shadow/IB memory between CPU writes and GPU reads (Garlic: L1 not coherent; L2 doesn't see CPU writes).
   - Uncached slot arena.
   - ACQUIRE_MEM with K$+TCL1 always, plus L2 invalidate at SYSTEM scope.
   - Add `HSA_SOFT_AQL_FORCE_L2_INV=1` for debugging stale-kernarg symptoms.
   - Per-dispatch L2 invalidation may cost real throughput. Measure it.
4. **Partial-workgroup semantics with USE_THREAD_DIMENSIONS**: untested. There is a PARTIAL_TG_EN fallback.
5. **HIP generic pointers**: see §8.6. This is the risk most likely to limit HIP to "simple kernels only".
6. ROCr code that assumes `AqlQueue` (the `aql_queues_` casts, `InterceptQueue::GetInfo`) is localised and listed in §1.
