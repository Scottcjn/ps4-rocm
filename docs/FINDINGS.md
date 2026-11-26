# Findings log

All of this was measured on one PS4 Pro: GoldHEN 12.00, Arch Linux, Strawberry kernel `7.3.4-Strawberry-Server-FullLTO`, Mesa 26.3.1. Unless a line says otherwise, nothing here has been verified on a base PS4.

## Hardware as Linux sees it (PS4 Pro)

| | |
|---|---|
| CPU | 8× AMD Jaguar, 800–2100 MHz (2.1 GHz = Pro; base/Slim = 1.6), AVX (no AVX2) |
| GPU | Gladius `1002:9924`, driven by amdgpu's gfx_v7 IP blocks. CP firmware `gladius_b` |
| CUs | **36**, measured via `VK_AMD_shader_core_properties`: 4 SE × 9 CU, 4 SIMD/CU, wave64 |
| Memory | 8 GB GDDR5 unified; Linux sees 7.6 GiB. 128 MiB VRAM carve-out + 3.9 GiB GTT. Vulkan heaps: 2682 MiB device-local, 1341 MiB host |
| Vulkan | RADV reports it as `AMD GL-B2 (RADV KAVERI)`, Vulkan 1.3. int8 yes, **FP16 not exposed** (the Pro's packed FP16 is hidden behind the gfx7 identity) |
| Disk | Stock PS4 Pro drive: HGST Travelstar 5K1000 `HTS541010B9E380`, 1 TB 2.5" SATA, **mechanical** (`rotational=1`; 5400 rpm per the model number, not measured) |
| KFD | **Off** in stock Strawberry builds (`# CONFIG_HSA_AMD is not set`) |

## 1. Vulkan compute hangs: root cause = FLAT

Symptom: `llama-bench -ngl 99` never finishes. dmesg repeats `amdgpu: ring comp_1.0.0 timeout, but soft recovered`.

Ruled out:
- **Queue type:** `RADV_DEBUG=nocompute` moves the hang to `ring gfx`.
- **int-dot:** the hang persists with it disabled.
- **llama.cpp shaders:** `test-backend-ops -o ADD` dies in `ggml_vk_buffer_read`, before any ggml shader is compiled.

`tools/vkbisect` (every combination of compute/graphics queue × host/local memory):

| test | result |
|---|---|
| fill (RADV meta shader → `flat_store_dwordx2`) | HANG, data untouched |
| copy (RADV meta shader) | HANG, data untouched |
| trivial SSBO compute shader (`buffer_store_dword`) | OK, 4096/4096 correct |

`RADV_DEBUG=shaders,metashaders` confirms that the fill shader is FLAT and our shader is MUBUF.

Independent confirmation: Garlic LLM compiles PS4 kernels with `-target-feature -flat-for-global`, with the comment "FLAT instructions hang the PS4's GPU". That observation comes from Sony's OS, not Linux, which suggests a hardware property rather than a Linux setup bug.

Other PS4 GPU rules documented by Garlic and not yet tested on Linux:
- `ds_read_b128` / `ds_write_b128` return wrong LDS data.
- Scratch / register spills are unsupported.
- L1 is not coherent between dispatches.
- The L2 does not see CPU writes without an invalidate.

## 2. KFD (ROCm kernel driver)

The Strawberry 7.3.4 tree already lists `CHIP_LIVERPOOL` / `CHIP_GLADIUS` in:
- the KFD CRAT cache info (marked with a TODO),
- the aperture setup (`kfd_flat_memory.c`),
- the packet manager and the queue manager.

**Only `kgd2kfd_probe()` was missing**, so KFD never initialised.
`kernel/0001-amdkfd-register-ps4-liverpool-gladius.patch` registers both chips:
- as gfx701 (Hawaii-style dGPU path, which avoids the Kaveri APU / IOMMUv2-ATS path),
- with CWSR off (no gfx7 trap-handler firmware),
- with PCIe atomics not required (internal APU bus).

Also `kernel/0002-…`: the existing PS4 line in `cik_event_interrupt.c` has an unbalanced parenthesis.
That code is never compiled in stock builds (KFD off), so nobody noticed it. The patch fixes it and adds Gladius next to
Liverpool/Hawaii in the "VMID/PASID not written into IH ring entry" workaround (assumed for Gladius, not observed).
With both patches the whole amdgpu/amdkfd directory compiles cleanly (clang 20, LLVM=1).

Pre-boot review (Grok read the tree; a research agent read the tree and upstream ROCm independently) found
two more never-run problems, both fixed:
- `0003`: in no-HWS mode (forced for these chips), `start_nocpsch()` initialised the packet manager only for
  Hawaii, but the dequeue TC flush ran for Liverpool too, which is a **NULL `pmf` dereference** (kernel oops) on the
  first queue destroy. Gladius was missing from both. One helper now keeps flush and `pm_init`/`pm_uninit` in sync.
- `0004`: `kgd_set_pasid_vmid_mapping()` spun **forever** waiting for the ATC handshake, which no PS4 had ever
  exercised. It's now bounded by `usec_timeout`; on timeout it warns once and continues. ATC is off in the MQD
  (GPUVM path), and the IH LUT, which KFD events need, is still programmed.
Checked and fine: doorbell BAR 2 is 8 MiB; MSI interrupts are enabled (`PS4 GPU IRQ: … msi_enabled=1`); KFD forces
the SW scheduler (no HWS) for these chips; VMIDs are split 8-15 for KFD. `AMD_IS_APU` is set, so KFD takes the modern
APU memory path (GTT as public local memory, no IOMMUv2/ATS).

Status: **BOOTED, and KFD works on the PS4 Pro (2026-11-25).** As far as we know, this is the first KFD on a PS4. Kernel `7.3.4-Strawberry-Server-FullLTO+ #2`
with patches 0001–0004, launched by sending GoldHEN's BinLoader (port 9090) `/data/payloads/linux-128mb.elf`.

```
kfd kfd: Allocated 3969056 bytes on gart
kfd kfd: Total number of KFD nodes to be created: 1
amdgpu: SW scheduler is used
kfd kfd: added device 1002:9924
/dev/kfd  crw-rw-rw- root render
```
KFD topology node 1, `gladius`:
- `gfx_target_version 70001`, `simd_count 144` (= 36 CU × 4), wave 64, LDS 64 KB.
- `fw_version 421`, `num_sdma_engines 2`, `num_cp_queues 24`.
- One public mem bank of 3.9 GB (GTT, the APU path).
- `capability 0x280`: doorbell type 0 (legacy), so ROCr **6.4.4** is the last runtime that accepts it.

Cosmetic: `cu_per_simd_array 10` is the max, not the 9 active. No KFD errors, no oops, and no ATC-timeout warning at boot.
Regression check on the new kernel (patched Mesa):
- vkbisect is all OK and there are 0 GPU timeouts.
- llama-bench 0.6B: tg32 is unchanged (23.0 tok/s), but **pp128 dropped 1164 → 790 tok/s**. Not explained yet; suspects are the GPU clock state or the KFD GTT reservation.

Building the kernel needs the firmware blobs that the running kernel has built in (`mrvl/sd8797_uapsta.bin` can't be downloaded by `build.sh`). `tools/extract-builtin-fw.py` lifts all 37 of them byte-for-byte from the running `bzImage`.

## 2b. Mesa ACO patch: no FLAT, no 128-bit LDS on PS4 (tested on Gladius)

A PCI-ID-gated (0x9920, 0x9924) `has_flat_hang_bug` / `has_lds_b128_bug` in `ac_gpu_info`:
- ACO lowers global memory to MUBUF addr64 (the GFX6 path), not FLAT.
- LDS accesses wider than 64 bits are split.

The patch is written but not yet in this repo.

| test (patched RADV) | before | after |
|---|---|---|
| vkbisect, all 12 combinations | 8 HANG | **12 OK, 0 GPU timeouts** |
| meta fill shader ISA | `flat_store_dwordx2` | `buffer_store_dwordx2` |
| llama.cpp `test-backend-ops -o ADD` | device lost | **173/173 passed** |
| llama.cpp `test-backend-ops -o MUL_MAT` | device lost | **2636/2636 passed** (after the queue fix below) |

**Second bug: the compute queues.** After the FLAT fix, one MUL_MAT job still hung, always on a compute ring
(`ring comp_1.0.1 timeout`, hard reset), and always at the same submission in the full run. It passed alone.
- llama.cpp used RADV's compute queue plus the GFX queue (transfers via meta compute shaders).
- The hang didn't follow shader code, ISA, buffers or VAs. It moved when the submission layout changed, and it
  vanished when every submit was waited on, or when everything ran on the GFX ring.

Fix (in the Mesa patch): RADV exposes no compute queue on the PS4 PCI IDs. The root trigger is not identified (hypothesis:
compute rings + GFX-ring compute on the same context). Upstream already disables compute queues on GFX6.

### First llama.cpp numbers on the PS4 Pro GPU (2026-11-25, llama.cpp 244640b, patched Mesa 26.3.1)

`llama-bench -p 128 -n 32`. The GPU column is `-ngl 99`; the CPU column is `-ngl 0 -t 8` with the GPU hidden.

| model | test | GPU (tok/s) | CPU 8×Jaguar (tok/s) |
|---|---|---|---|
| Qwen3 0.6B Q8_0 | pp128 | **1164** | 54 |
| Qwen3 0.6B Q8_0 | tg32 | 22.9 | 22.9 |
| Qwen3 4B Q4_K_M | pp128 | **94** | — |
| Qwen3 4B Q4_K_M | tg32 | 6.7 | 5.6 |

Prompt processing is ~21× the CPU. **Generation is barely faster than the CPU.** 6.7 tok/s × 2.3 GB ≈ 16 GB/s effective,
versus ~218 GB/s of GDDR5, and Garlic reports ~45 tok/s on a 9B on this console. Generation is limited by something other than
bandwidth. Suspects:
- per-token submit latency,
- the single GFX queue,
- the GPU left at its boot clock (dmesg `SCLK … DID=16`, "leave strap").

That's the next optimisation target.

## 3. Platform gotchas (all handled in the scripts)

- Kernel build on Ubuntu: `LLVM=1` calls `llvm-objcopy` / `llvm-readelf`, which Ubuntu installs only as `*-20`.
  Put `/usr/lib/llvm-20/bin` first in `PATH` (`kernel/build-kfd-kernel.sh` does this).

- `vulkaninfo` crashes: with `amdgpu.dc=0` there is no display engine (`VK_KHR_display` → OUT_OF_HOST_MEMORY). Use `tools/vkprobe`.
- Fresh PS4 Arch images ship without a pacman keyring.
- The PS4 kernel is not a pacman package, so `pacman -Syu` cannot replace it.
- sshd stopped during the openssh upgrade and stayed down. Fixed with a `Restart=always` override.
- `echo pass | sudo -S tee file` in a pipeline writes **empty** files: stdin is shared between the password and the data. Use askpass (`node/lib.sh`).
- iwd deletes the driver's wifi interface (`mlan0`) and recreates it as `wlanN`.
- `iwctl … connect` blocks on a prompt over ssh. Use `--passphrase`.
- `pkill -f <pattern>` over ssh kills your own ssh shell. Use `pkill -x`.
- The PS4 user partition is `/dev/mapper/ps4hdd` (UFS2), already mounted rw by the initramfs. A second mount must reuse that superblock (`mount` + `remount,bind,ro`); a plain `-o ro` mount is refused.
- The kernel lives at `/data/linux/boot/bzImage` on that partition. The root filesystem is a loop image (`/user/app/linux.img`).

## 2c. ROCm 6.4.4 + kfdtest on KFD (2026-11-25): first KFD compute on a PS4

ROCr/libhsakmt `rocm-6.4.4` (the last release with the legacy doorbell path) and kfdtest were built on the Pro (clang/LLVM 23, cmake 4.5).
`rocm/rocr-6.4.4-ps4-gfx7.patch` holds the toolchain fixes and the kfdtest no-FLAT shader. The parts that matter:
- `kfdtest/ShaderStore.cpp`: `CopyDwordIsa` gfx7 → `buffer_load/store_dword … addr64`, rsrc `{0,0,0,0x100f000}`.
- (The libhsakmt CWSR workaround the first run needed is gone: kernel `0005` fixes it. See below.)

| kfdtest | result |
|---|---|
| `KFDTopologyTest.*` (7) | PASS |
| `KFDQMTest.CreateDestroyCpQueue` | PASS (once the CWSR bug is worked around; see 0005) |
| `KFDQMTest.SubmitNopCpQueue` | PASS (12 ms) |
| `KFDQMTest.EmptyDispatch` | **PASS (13 ms): first wave launch under KFD; the EOP event arrived** |
| `KFDQMTest.SimpleWriteDispatch` (no-FLAT shader) | **PASS: a shader read and wrote memory correctly through a KFD PM4 queue** |
| `KFDQMTest.SubmitPacketCpQueue` | PASS |
| `KFDQMTest.CreateAqlCpQueue` | PASS (create/destroy only) |

HSA: `hsa_init` works and lists the agent as **`gfx701`, 36 CUs**. A clang `-mcpu=hawaii` no-FLAT code object compiles clean.
**Blocker: AQL.** ROCr's first GPU work (`hsa_executable_freeze` → `InvalidateCodeCaches` → PM4 inside the AQL utility queue)
never retires. A probe showed the MEC executed the slot's PM4 and IB, but it **never advanced the read index**. The
gladius MEC firmware apparently doesn't implement AQL (`RELEASE_MEM` event index 7 / AQL packet processing).
Result: `cp queue preemption time out` → `Failed to evict process queues` → after 3 hangs a kfdtest process stuck in D
state, and KFD needed a reboot. Vulkan still worked.
**So:** stock ROCr can't dispatch on this firmware. The way forward is a **PM4-queue dispatch path**: an
HSA-compatible bridge that submits code objects the way kfdtest does (`DISPATCH_DIRECT` on a PM4 compute queue).

Kernel side:
- `0005`: `kfd_queue_acquire/release_buffers()` looked up a CWSR save area even on nodes with `cwsr_size 0`,
  so every compute queue create failed with `EINVAL`. Skipped now.
- The ATC handshake works: there was no `ATC PASID mapping not acked`.
- One unexplained `WARN_ONCE` happened during the first queue run: `FW bug: No PASID in KFD interrupt` (vmid 8, pasid 0).
  It did **not** recur on kernel v3.
- **0005 validated (kernel v3, build #3, booted via GoldHEN BinLoader):** with stock libhsakmt (no workaround),
  CreateDestroyCpQueue 48 ms, SubmitNopCpQueue 12 ms, EmptyDispatch 229 ms and SimpleWriteDispatch 58 ms all PASS,
  with 0 GPU timeouts and no new kernel warnings.

## 2d. pm4bridge: clang kernels on KFD PM4 queues (2026-11-25), `bridge/pm4bridge/`

A C library (`pm4_init/alloc/load/dispatch/wait`) that loads a clang `-mcpu=hawaii` no-FLAT code object (`.text` + `.kd`) and
dispatches it with `DISPATCH_DIRECT` on a KFD PM4 compute queue (kfdtest-derived, MIT). All tests pass, **0 hangs**:

| test | result |
|---|---|
| noop | ~10 µs round trip |
| fill ×2 (stale-cache check), vadd 16M, matvec 4096² | bit-exact vs CPU |
| stress 2×1000 dispatches | none lost; 10.7 µs sync / 7.6 µs async per dispatch |
| errors | 20 misuse cases rejected; a scratch-needing kernel is refused at load |
| **copy/read bandwidth** | **read 18.2 GB/s, write 7.3 GB/s**, copy 9.1 GB/s (CPU memcpy 5.25 GB/s) |
| matvec f32 | 9.0 GFLOP/s at 18.0 GB/s (column-major); CPU 1 thread 1.25 GFLOP/s |

**The GPU memory path tops out at ~18 GB/s read**, which matches llama.cpp generation (~16 GB/s effective). This is the
real token-generation bottleneck. More waves, 16-byte loads, pinned GTT and CU masks all leave it unchanged.
**SOLVED: it's CPU-cached (snooped) vs write-combined memory, not VRAM vs GTT** (`tests/t_vrambw`, `vk/vkbw`, 2026-11-25).

| memory | GPU read | write | copy |
|---|---|---|---|
| CPU-cached GTT / userptr (KFD default, llama.cpp default) | 18 GB/s | 7.3 | 9.0 |
| real VRAM carve-out (Vulkan DEVICE_LOCAL) | 161–164 | 107 | 57 |
| **write-combined GTT** (`AMDGPU_GEM_CREATE_CPU_GTT_USWC`, or Vulkan HOST_VISIBLE non-cached) | **167–176** | **111** | **56** |

Cached pages go through the GPU's coherent (snooped) path; WC pages and VRAM don't. **No bigger VRAM carve-out is needed.**
pm4bridge gained `pm4_import_dmabuf()`: allocate a GTT BO with USWC via libdrm, export it as a dmabuf, import it into KFD.

### llama.cpp: one env var, ~3× token generation
`GGML_VK_PREFER_HOST_MEMORY=1` puts weights in the host-visible, non-cached memory type, i.e. the fast path:

| model | test | default | **PREFER_HOST_MEMORY=1** |
|---|---|---|---|
| Qwen3 0.6B Q8_0 | tg32 | 22.9 | **73.3 tok/s** |
| Qwen3 0.6B Q8_0 | pp128 | 790 | 853 |
| Qwen3 4B Q4_K_M | tg32 | 6.6 | **20.5 tok/s** |
| Qwen3 4B Q4_K_M | pp128 | 94 | 142 |

(`GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM=1` doesn't help: 6.6.) Output is verified correct (4B: "Paris … the Seine"), with 0 GPU timeouts.
Always run with: `VK_ICD_FILENAMES=$HOME/mesa-ps4/share/vulkan/icd.d/radeon_icd.x86_64.json GGML_VK_PREFER_HOST_MEMORY=1`.
For comparison, Garlic (native, hand-written kernels) reports ~45 tok/s on a 9B on this console. Next: check whether the 4B is now
compute-limited (20.5 tok/s × 2.3 GB ≈ 47 GB/s, well under 165 GB/s).

Other findings:
- kfdtest's `DISPATCH_INITIATOR 0x21` makes gfx7 treat `dim_x` as a *thread* count; the correct value is `0x1` (Garlic's).
- `buffer_*_dwordx4` (global) is fine; the b128 bug is LDS-only.
- GPU timestamps tick at ~100 MHz.

## 2e. How big a model fits on one PS4 Pro (2026-11-25)

Same setup as above: patched Mesa ICD + `GGML_VK_PREFER_HOST_MEMORY=1`, llama.cpp 244640b, `llama-bench -r 1`
(single runs; treat ±10% as noise). Each run was followed by a GPU health check (`vkbisect`) and a hang/oops count;
none changed. The 8 GB is shared by CPU, GPU and the OS, so "GPU layers" doesn't free RAM — every layer lives in
the same memory, and the rest spills to the HDD swap.

| Model | Size | GPU layers | pp128 tok/s | tg32 tok/s |
|---|---|---|---|---|
| Qwen3-4B Q4_K_M | 2.32 GiB | all | 142 | 20.5 |
| Qwen3-8B Q3_K_S | 3.51 GiB | all | 70 | **9.3** |
| Granite 3.3 8B Q3_K_S | 3.34 GiB | all | 61 | 8.3 |
| Qwen3-8B Q4_K_M | 4.68 GiB | 30 of 36 | 22 | 7.7 |
| Qwen3-8B Q4_K_M | 4.68 GiB | all | — | `Not enough memory for command submission` → DeviceLost |
| Phi-4 14B Q3_K_S | 6.05 GiB | 18 | 19 | 2.1 |
| Qwen3-14B Q4_K_M | 8.38 GiB | 0 | 1.1 | 0.01 (tg8; HDD-bound) |

Takeaways:
- **~8B at Q3 is the single-console ceiling** (~9 tok/s, fully on the GPU) *for standard quants*. Ternary
  quants move it: a 9B at 2.25 GiB runs fully on the GPU at 14.2 tok/s (§2f).
- 8B Q4_K_M fully offloaded fails at command submission, not at load: the GPU-visible budget (~2.6 GiB device-local +
  host heap) runs out once the KV cache and compute buffers are added. Partial offload (30/36) works.
- 14B doesn't fit in RAM at all; it pages from the HDD. **Bigger models need more consoles** → llama.cpp RPC split
  across PS4s (next).

## 2f. Round 2: current small models (2026-11-25/26)

PrismML's llama.cpp fork (`f702476`, branch `prism`) ships Vulkan shaders for its ternary formats. They run on gfx7 with
the patched Mesa, so ternary models stay on the GPU instead of falling back to the CPU. Same env as §2e, `-r 2`.
Full rows: [ps4-ai-benchmarks](https://github.com/Scottcjn/ps4-ai-benchmarks) `results/llm.csv`.

| Model | Size | pp128 | tg32 | note |
|---|---|---|---|---|
| Ternary-Bonsai-2-2B PQ2_0 | 0.48 GiB | 368 | **43.0** | 16k ctx in 988 MiB GTT |
| Gemma-4-E2B-it IQ4_XS | 2.78 GiB | 255 | **27.0** | best overall; 3/3 smoke prompts |
| Qwen3.5-4B Q4_0 | 2.41 GiB | 157 | 23.7 | |
| Qwen3-4B Q4_K_M (round-1 model, this build) | 2.32 GiB | 137 | 20.1 | build ~3% slower than 244640b |
| **Ternary-Bonsai-2-9B PQ2_0** | 2.25 GiB | 86 | **14.2** | **a 9B fully on the GPU**; 1.5× the best 8B Q3 |
| Gemma-4-E4B-it Q3_K_S | 3.60 GiB | 114 | 11.1 | needs `-lm mmap` |
| Qwen3.5-9B Q3_K_S (ngl 28) | 4.02 GiB | 40 | 5.8 | over the GTT cap |

- **The fork loads without mmap by default.** Near the ~3.9 GiB cap that fails with `Not enough memory for command submission` → DeviceLost. Use `-lm mmap`.
- There were 12 such DeviceLost events in the run. All were in user space, with 0 kernel GPU hangs.
- **Bits per weight is the lever on this console, not compute.** At 2.25 GiB the 9B ternary model is smaller than a 4B at Q4.

## 2g. Regression check, 2026-11-26: the KFD queue hangs under memory pressure

The pm4bridge `make check` ran on the same kernel and boot as §2d's all-pass run (2026-11-25 22:00).
- `noop`, `errors` and both `fill` runs passed.
- **`t_vadd` hung.** It needs 3 × 64 MiB GTT buffers. Its fence never retired: `rptr 0, wptr 11`, so the CP never read the queue.
- Teardown then logged `cp queue preemption time out` → `Failed to evict process queues` → `Failed to quiesce KFD` (3 preemption timeouts).
- No process was left in D state, and the miner kept running.

State at the time:
- Only 343 MiB of RAM was free; 7.2 GiB was page cache, mostly model files from §2f.
- §2f's runs had also left 12 DeviceLost events in user space.

**One variable changed:** the model files' page cache was dropped as the normal user (`posix_fadvise(DONTNEED)`, no root), leaving 3.8 GiB free.
- `t_vadd` then **PASSED**: bit-exact, 18.6 GB/s.
- The rest of the suite then passed with numbers matching §2d: stress 7.6 µs/dispatch, read 18.2 GB/s, write 7.3 GB/s, matvec 9.1 GFLOP/s, wide 18.2 GB/s.
- The trouble count stayed at 5 throughout.

**Working hypothesis (n = 1 fail, 1 pass; not yet reproduced on purpose):**
1. A KFD allocation that forces page-cache reclaim triggers a KFD eviction of the process's queues.
2. On gfx7 no-HWS, the restore never re-maps the queue, so the next dispatch is never fetched.

To confirm: refill the page cache (`cat` the model files), then run `t_vadd` once.
- If it hangs again, trace `kfd_process_evict_queues` / `restore_process_worker`.
- That run risks another hang. The 2026-11-25 notes record a D-state process after 3 hangs, which needed a reboot, and reboots go through GoldHEN.

**Until fixed:** run KFD/pm4bridge work with RAM free. Drop model page cache first. Don't run it right after big llama.cpp sessions.

## 4. Installing a test kernel from Linux (done on the Pro, 2026-11-25)

The payload hardcodes `bzImage` (it checks USB0, then USB1, then `/data/linux/boot`). We swapped from the running Linux
through the shared `/dev/mapper/ps4hdd` UFS2 mount (`CONFIG_UFS_FS_WRITE=y`, which the root image already relies on):
1. Verify the current `bzImage` against the backup.
2. Write `bzImage-new`, `sync`, drop caches, verify sha256 from disk.
3. Rename `bzImage` → `bzImage-stock` and `bzImage-new` → `bzImage`; verify again from disk.

Zero UFS/I/O errors in dmesg. Rollback: rename back from Linux, or over GoldHEN FTP with `tools/ps4-ftp-kernel.sh rollback`.
The initramfs only *mounts* an existing `/ps4hdd/app/linux.img`. Reinstalling (`install-linux-int.sh`) is manual and
requires typing `YES`, so reboots don't wipe the install.

## Next

1. ~~Boot the KFD kernel~~ ✅ booted. ~~kfdtest~~ ✅ (§2c).
2. ~~Mesa ACO patch~~ ✅ (§2b): vkbisect 12/12 OK, llama.cpp runs.
3. ROCr user space: ✅ `hsa_init` sees gfx701. ❌ Stock ROCr can't dispatch, because the MEC firmware has no AQL (§2c).
   ✅ pm4bridge dispatches clang kernels on PM4 queues instead (§2d), re-verified 2026-11-26 (§2g).
   Open: a HIP-like front end over pm4bridge.
3b. **Fix the KFD queue hang under memory pressure (§2g)**: reproduce once, then trace evict/restore on no-HWS gfx7.
3c. Explain the pp128 drop on the KFD kernel (1164 → 790, §2); tg is unchanged.
4. Benchmarks: Linux Vulkan vs Garlic on the same Pro, same model.
5. Expose the Pro's packed FP16 to RADV/LLVM.
6. Two-PS4 split: `rpc-server` on a second console, head node on the Pro → run a 14B across both (§2e).
