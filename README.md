# PS4 ROCm — GPU compute for PlayStation 4 / PS4 Pro on Linux

Run AI models on the PS4's GPU under Linux. The work has three parts:

1. **ROCm / KFD** for the PS4 GPUs (Liverpool = PS4/Slim, Gladius = PS4 Pro). As far as we can find, this is the first public attempt.
2. **Vulkan (Mesa RADV) fixes** so that llama.cpp and other Vulkan compute stops hanging the GPU.
3. **Cluster tooling**: repeatable installers that turn any jailbroken PS4 running Linux into an inference node. A PS4 Pro acts as the head node.

> Status (2026-11-25):
> - **KFD boots on a PS4 Pro.** `/dev/kfd` is up and the GPU is listed as gfx701 with 36 CUs.
> - **llama.cpp runs on the PS4 Pro GPU** through a patched Mesa RADV: MUL_MAT 2636/2636, 0.6B prompt processing ~0.8–1.2k tok/s.
> - **Next:** ROCm 6.4.4 user space. See [docs/FINDINGS.md](docs/FINDINGS.md).

## Key finding: FLAT memory instructions hang the PS4 GPU

On a PS4 Pro (Gladius, `1002:9924`, 36 CUs) running Strawberry Linux 7.3.4 + Mesa 26.3.1, `tools/vkbisect` gives:

| Operation | GCN instruction | Result |
|---|---|---|
| `vkCmdFillBuffer` / `vkCmdCopyBuffer` (RADV internal shaders) | `flat_store_dwordx2` | **GPU hang** (`ring timeout, soft recovered`) |
| Trivial compute shader on a descriptor SSBO | `buffer_store_dword` | **Correct** (4096/4096) |

Stock llama.cpp dies on its first buffer copy, before it runs a single model shader. Garlic LLM documents the same hardware rule: it compiles its PS4 kernels with `-target-feature -flat-for-global`.

The fix direction:
- **Vulkan:** Mesa's ACO compiler must lower global memory on gfx7 to `MUBUF addr64` (it already does this for gfx6) instead of `FLAT`.
- **ROCm:** LLVM must compile with `-mattr=-flat-for-global`, and no generic-pointer access may remain.

## Layout

| Path | What |
|---|---|
| `host/setup-host-link.sh` | Shares a Linux host's network to PS4 nodes over ethernet (NetworkManager shared mode + ufw) |
| `host/add-node.sh` | Adds one PS4: DHCP pin, ssh alias, key install, pushes and runs the node installer |
| `node/detect.sh` | Detects PS4 model (Pro vs base, from GPU chip + CPU clock + southbridge), distro, privileges, RAM, KFD |
| `node/install-node.sh` | Node setup: packages (pacman/apt), self-healing sshd, GPU probe, llama.cpp (Vulkan + RPC) |
| `node/setup-wifi.sh` | Built-in Marvell wifi via iwd + systemd-networkd, DHCP or static |
| `node/lib.sh` | Shared helpers (askpass-based sudo, checked root file writes) |
| `tools/vkprobe.c` | Headless Vulkan probe (vulkaninfo crashes with `amdgpu.dc=0`); reports CU count |
| `tools/vkbisect.c` | Finds the first GPU operation that hangs (fill / copy / shader × queue × memory type) |
| `tools/extract-builtin-fw.py` | Dumps `CONFIG_EXTRA_FIRMWARE` blobs from a vmlinux (to rebuild a kernel that boots the same hardware) |
| `tools/fetch-model.sh` | Downloads a GGUF together with its LICENSE, model card and a revision-pinned receipt |
| `kernel/0001-amdkfd-register-ps4-liverpool-gladius.patch` | Registers the PS4 GPUs with KFD (`/dev/kfd`) |
| `kernel/build-kfd-kernel.sh` | Builds a Strawberry kernel with KFD from a node's running config |

## Quick start (one PS4)

```bash
# on the Linux host, PS4 cabled to its ethernet port
host/setup-host-link.sh eno1
NODE_PASS='<ps4 user password>' host/add-node.sh thebe <ps4-mac> 10.42.0.85 ps4
# on the node afterwards
ssh thebe 'cat ~/node-info.txt'
```

Requirements: a PS4 / PS4 Pro already running Linux (e.g. Strawberry kernel + Arch). This repo doesn't cover jailbreaking or booting Linux; the PS4 Linux community covers that.

## Credits

- **Garlic LLM** (Leonardo Alegría, GPLv3): runs Qwen LLMs natively on the PS4 GPU. Its source documents the PS4 GPU's rules (no FLAT, no `ds_read_b128`, no scratch, non-coherent L1). We relied on those documented findings; no Garlic code is included here.
- **Strawberry PS4 kernel** by rmux ([gitlab.com/rmuxnet/linux](https://gitlab.com/rmuxnet/linux)), which already carries most of the PS4 KFD plumbing.
  `kernel/*.patch` is a small change **to** Strawberry, not a fork of it. It's meant to go upstream as a merge request there.
  Build Strawberry from its own repo; this project doesn't ship kernels.
- **fail0verflow**, for the original PS4 Linux and Radeon bring-up.

## License

MIT for this repository's own code (see `LICENSE`), except `kernel/*.patch`, which modifies the Linux kernel and is GPL-2.0.
No firmware blobs and no model weights are distributed here.
