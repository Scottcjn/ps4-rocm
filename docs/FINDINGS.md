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

Status: **4 patches, compiles cleanly, not yet booted.**

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
| llama.cpp `test-backend-ops -o MUL_MAT` | device lost | 2359 cases OK, then **one case hangs** (hard reset) |

The remaining MUL_MAT hang is being bisected: something besides FLAT/b128, possibly `ds_read2_b64`, subgroup ops, or
large-workgroup shaders.

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

## Next

1. Boot the KFD kernel next to the original (keep `bzImage` untouched) → check `/dev/kfd` and the KFD topology.
2. Mesa ACO patch: gfx7 global memory → MUBUF addr64; no `ds_*_b128` → rerun vkbisect + llama-bench.
3. ROCr/ROCT + LLVM gfx701 with `-flat-for-global` off → hello-world HIP/OpenCL kernel.
4. Benchmarks: Linux Vulkan vs Garlic on the same Pro, same model.
5. Expose the Pro's packed FP16 to RADV/LLVM.
