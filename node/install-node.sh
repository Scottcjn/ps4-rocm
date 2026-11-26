#!/usr/bin/env bash
# install-node.sh — turn a PS4 / PS4 Pro running Linux into an AI bench node.
# Runs ON the PS4 as the normal user (needs sudo). Safe to re-run: every step checks first.
#
#   usage:  SUDO_PASS='...' ./install-node.sh     full install
#           SKIP_BUILD=1 ./install-node.sh        packages + probes only
#           FORCE=1 ./install-node.sh             run even if detect.sh says "not a PS4"
#           JOBS=2 LLAMA_REF=<sha> ...            override build jobs / pin llama.cpp
#
# Steps:
#   0. detect.sh — which PS4 (Pro/base), distro, privileges, RAM -> build jobs, kernel features
#   1. package-manager prep (Arch: init pacman keyring — fresh PS4 images ship without one)
#   2. packages (pacman or apt list chosen by detection)
#   3. sshd: enabled + Restart=always override (no OOMScoreAdjust: ssh children inherit it) (on thebe sshd stopped cleanly during the
#      openssh upgrade and stayed down — no OOM involved)
#   4. vkprobe — headless Vulkan probe (vulkaninfo crashes when amdgpu.dc=0)
#   5. llama.cpp with the Vulkan backend
#   6. ~/node-info.txt — detection + probe snapshot for the cluster inventory
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LLAMA_DIR="${LLAMA_DIR:-$HOME/llama.cpp}"
LLAMA_REF="${LLAMA_REF:-master}"

. "$HERE/lib.sh"

# ---- 0. detection -------------------------------------------------------------------
log "0/6 detect"
eval "$(bash "$HERE/detect.sh")"
bash "$HERE/detect.sh" | sed 's/^/  /'
[ "$IS_PS4" = yes ] || [ -n "${FORCE:-}" ] || die "this doesn't look like a PS4 (set FORCE=1 to override)"
[ "$MODEL_CONFLICT" = no ] || echo "WARN: model signals disagree ($MODEL_CONFLICT) — check by hand"
[ "$RENDER_NODE" = yes ] || echo "WARN: no /dev/dri/renderD128 — amdgpu not up, GPU steps will fail"
JOBS="${JOBS:-$BUILD_JOBS}"

setup_sudo

# ---- 1-2. packages ------------------------------------------------------------------
case "$PKG" in
  pacman)
    log "1/6 pacman keyring"
    if [ ! -d /etc/pacman.d/gnupg ]; then
      sudo_ pacman-key --init
      sudo_ pacman-key --populate archlinux
    else echo "  keyring present"; fi
    log "2/6 packages (pacman)"
    # NB: PS4 kernels are not pacman packages, so -Syu cannot replace the PS4 kernel.
    sudo_ pacman -Syu --noconfirm --needed \
      base-devel git cmake ninja python python-numpy htop rsync \
      mesa vulkan-radeon vulkan-tools vulkan-headers shaderc glslang spirv-headers \
      opencl-mesa ocl-icd clinfo radeontop
    ;;
  apt)
    log "1/6 apt update"
    sudo_ apt-get update
    log "2/6 packages (apt)"
    # NB: check that no linux-image-* upgrade would replace the PS4 kernel before running this.
    sudo_ env DEBIAN_FRONTEND=noninteractive apt-get install -y \
      build-essential git cmake ninja-build python3 python3-numpy htop rsync \
      mesa-vulkan-drivers vulkan-tools libvulkan-dev glslc glslang-tools spirv-headers \
      mesa-opencl-icd ocl-icd-libopencl1 clinfo radeontop
    ;;
  *) die "unsupported package manager ($PKG) — add a package list for $DISTRO" ;;
esac

# ---- 3. sshd ------------------------------------------------------------------------
log "3/6 sshd at boot + self-healing"
SSHD_UNIT=sshd; systemctl list-unit-files sshd.service >/dev/null 2>&1 || SSHD_UNIT=ssh   # Debian calls it ssh
sudo_ mkdir -p "/etc/systemd/system/$SSHD_UNIT.service.d"
# NO OOMScoreAdjust here: every process started over ssh inherits it, so on 2026-11-25 an
# OOM killed journald/networkd/the miner (16 procs) and spared the runaway ssh-launched benchmark.
printf '[Service]\nRestart=always\nRestartSec=3\n' | \
  write_root "/etc/systemd/system/$SSHD_UNIT.service.d/override.conf" 644
sudo_ systemctl daemon-reload
sudo_ systemctl enable --now "$SSHD_UNIT"

# ---- 4. vkprobe ---------------------------------------------------------------------
log "4/6 vkprobe"
cc -O2 -o "$HOME/vkprobe" "$HERE/../tools/vkprobe.c" -lvulkan
"$HOME/vkprobe" || echo "WARN: vkprobe failed — GPU not visible to Vulkan"

# ---- 5. llama.cpp -------------------------------------------------------------------
if [ -z "${SKIP_BUILD:-}" ]; then
  log "5/6 llama.cpp (Vulkan, $JOBS jobs)"
  [ -d "$LLAMA_DIR/.git" ] || git clone https://github.com/ggml-org/llama.cpp "$LLAMA_DIR"
  git -C "$LLAMA_DIR" fetch --depth 1 origin "$LLAMA_REF" && git -C "$LLAMA_DIR" checkout -q FETCH_HEAD
  cmake -S "$LLAMA_DIR" -B "$LLAMA_DIR/build-vk" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DGGML_VULKAN=ON -DGGML_RPC=ON -DGGML_NATIVE=ON -DLLAMA_CURL=OFF
  cmake --build "$LLAMA_DIR/build-vk" -j"$JOBS" --target llama-bench llama-cli llama-server rpc-server
else
  log "5/6 llama.cpp build skipped"
fi

# ---- 6. node-info -------------------------------------------------------------------
log "6/6 node-info"
{
  echo "# $(cat /etc/hostname 2>/dev/null) $(date -u +%FT%TZ)"
  bash "$HERE/detect.sh"
  echo "MESA=$(pacman -Q mesa 2>/dev/null || dpkg-query -W -f='${Version}' mesa-vulkan-drivers 2>/dev/null || echo unknown)"
  echo "LLAMA_CPP=$(git -C "$LLAMA_DIR" log -1 --format=%h 2>/dev/null || echo none)"
  "$HOME/vkprobe" 2>&1 | sed 's/^/# vk: /'
} | tee "$HOME/node-info.txt"
log "done — $PS4_MODEL node ready"
