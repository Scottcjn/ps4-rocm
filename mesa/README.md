# Mesa patches

`0001-radv-ps4-no-flat-no-lds-b128-gfx-queue-only.patch`: against mesa-26.3.1. Adds PCI-ID-gated
(`0x9920` Liverpool, `0x9924` Gladius) `has_flat_hang_bug` / `has_lds_b128_bug` / `has_compute_queue_hang_bug`:
- ACO lowers global memory to MUBUF addr64 (the GFX6 path) instead of FLAT, which hangs these GPUs.
- LDS accesses wider than 64 bits are split, because `ds_*_b128` return wrong data on these GPUs.
- RADV exposes no separate compute queue: compute-ring jobs hang (`ring comp_1.0.1 timeout`, hard reset) when the
  GFX ring is also running compute work. Everything goes to the GFX ring. This matches upstream's GFX6
  "compute queues may hang" policy. The exact trigger is unknown.

Build only RADV into a user prefix:

```
meson setup build -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled \
  -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dbuildtype=release --prefix=$HOME/mesa-ps4
ninja -C build -j6 install
export VK_ICD_FILENAMES=$HOME/mesa-ps4/share/vulkan/icd.d/radeon_icd.x86_64.json
```

Results are in `docs/FINDINGS.md` (section 2b). Status: llama.cpp `test-backend-ops` MUL_MAT **2636/2636**, ADD 173/173, no GPU timeouts; llama-bench runs on the GPU.
