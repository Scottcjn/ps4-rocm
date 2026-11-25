#!/usr/bin/env bash
# detect.sh — figure out what PS4 + Linux we're on, print KEY=VALUE lines.
# Used by install-node.sh (sourced via: eval "$(detect.sh)"), also runnable by hand.
# Never needs root. Every probe is best-effort; unknowns print "unknown", never fail.
#
# Signals (several, because no single one is trustworthy on PS4 Linux):
#   GPU chip   — amdgpu prints its chip name at init: LIVERPOOL = base/slim, GLADIUS = Pro
#   CPU clock  — Jaguar max 2100 MHz = Pro, 1600 MHz = base/slim
#   southbridge— Sony PCI 104d:xxxx: Aeolia (launch fat), Belize, Baikal (later units incl. Pro)
# The model is decided by GPU chip first, CPU clock second; disagreement is reported, not hidden.

set -u
v() { printf '%s=%q\n' "$1" "$2"; }

# ---- is this a PS4 at all? ---------------------------------------------------------
sony_pci="$(lspci -nn 2>/dev/null | grep -oiE '104d:[0-9a-f]{4}' | head -1)"
southbridge=unknown
for sb in Baikal Belize Aeolia; do
  lspci 2>/dev/null | grep -qi "$sb" && { southbridge=$sb; break; }
done
is_ps4=no
{ [ -n "$sony_pci" ] || grep -qi 'ps4' /proc/version 2>/dev/null || uname -r | grep -qiE 'ps4|strawberry'; } && is_ps4=yes

# ---- GPU chip ----------------------------------------------------------------------
gpu_pci="$(cat /sys/class/drm/card0/device/vendor /sys/class/drm/card0/device/device 2>/dev/null | tr -d '\n' | sed 's/0x//g; s/^\(....\)/\1:/')"
gpu_chip=unknown
# dmesg needs root on most distros; fall back to the lspci name
chip_line="$( (dmesg 2>/dev/null; journalctl -k -b --no-pager 2>/dev/null) | grep -oE 'initializing kernel modesetting \([A-Z]+' | head -1)"
[ -n "$chip_line" ] && gpu_chip="${chip_line##*(}"
if [ "$gpu_chip" = unknown ]; then
  lspci 2>/dev/null | grep -iE 'vga|display' | grep -qi gladius   && gpu_chip=GLADIUS
  lspci 2>/dev/null | grep -iE 'vga|display' | grep -qi liverpool && gpu_chip=LIVERPOOL
fi

# ---- CPU ---------------------------------------------------------------------------
cpu_max_mhz="$(lscpu 2>/dev/null | awk -F: '/CPU max MHz/{gsub(/ /,"",$2); printf "%d",$2}')"
[ -z "$cpu_max_mhz" ] && cpu_max_mhz=0
cpus="$(nproc 2>/dev/null || echo 1)"

# ---- model decision ----------------------------------------------------------------
model=unknown; model_conflict=no
case "$gpu_chip" in
  GLADIUS)   model=pro ;;
  LIVERPOOL) model=base ;;
esac
clock_says=unknown
if [ "$is_ps4" = yes ]; then   # clock heuristic only means anything on a Jaguar PS4
  [ "$cpu_max_mhz" -ge 2000 ] && clock_says=pro
  [ "$cpu_max_mhz" -gt 0 ] && [ "$cpu_max_mhz" -lt 2000 ] && clock_says=base
fi
if [ "$model" = unknown ]; then model=$clock_says
elif [ "$clock_says" != unknown ] && [ "$clock_says" != "$model" ]; then model_conflict="gpu=$model,clock=$clock_says"; fi

# ---- memory / GPU memory -------------------------------------------------------------
mem_mib="$(awk '/MemTotal/{printf "%d",$2/1024}' /proc/meminfo)"
vram_mib=$(( $(cat /sys/class/drm/card0/device/mem_info_vram_total 2>/dev/null || echo 0) / 1048576 ))
gtt_mib=$(( $(cat /sys/class/drm/card0/device/mem_info_gtt_total 2>/dev/null || echo 0) / 1048576 ))
# ~1.5 GiB per C++ compile job for llama.cpp's Vulkan backend; keep 2 GiB headroom
jobs=$(( (mem_mib - 2048) / 1536 )); [ "$jobs" -lt 1 ] && jobs=1; [ "$jobs" -gt "$cpus" ] && jobs=$cpus

# ---- kernel capabilities ---------------------------------------------------------------
kcfg() { (zcat /proc/config.gz 2>/dev/null || cat "/boot/config-$(uname -r)" 2>/dev/null) | grep -E "^$1=" | cut -d= -f2; }
kfd=no; [ -e /dev/kfd ] && kfd=yes
hsa_amd="$(kcfg CONFIG_HSA_AMD)"; [ -z "$hsa_amd" ] && hsa_amd=no
render_node=no; [ -e /dev/dri/renderD128 ] && render_node=yes
display_off=no; grep -q 'amdgpu.dc=0' /proc/cmdline 2>/dev/null && display_off=yes

# ---- distro / package manager --------------------------------------------------------
pkg=unknown
command -v pacman  >/dev/null 2>&1 && pkg=pacman
command -v apt-get >/dev/null 2>&1 && pkg=apt
command -v dnf     >/dev/null 2>&1 && pkg=dnf
distro="$(. /etc/os-release 2>/dev/null; echo "${ID:-unknown}")"

# ---- privileges ----------------------------------------------------------------------
priv=none
[ "$(id -u)" = 0 ] && priv=root
[ "$priv" = none ] && command -v sudo >/dev/null 2>&1 && id -nG | grep -qwE 'wheel|sudo' && priv=sudo

v IS_PS4 "$is_ps4"
v PS4_MODEL "$model"                 # pro | base | unknown
v MODEL_CONFLICT "$model_conflict"
v GPU_CHIP "$gpu_chip"
v GPU_PCI "$gpu_pci"
v SOUTHBRIDGE "$southbridge"
v CPU_MAX_MHZ "$cpu_max_mhz"
v CPUS "$cpus"
v MEM_MIB "$mem_mib"
v VRAM_MIB "$vram_mib"
v GTT_MIB "$gtt_mib"
v BUILD_JOBS "$jobs"
v KFD "$kfd"                         # yes = ROCm possible without a kernel rebuild
v CONFIG_HSA_AMD "$hsa_amd"
v RENDER_NODE "$render_node"
v DISPLAY_OFF "$display_off"         # amdgpu.dc=0 -> vulkaninfo crashes, use vkprobe
v PKG "$pkg"
v DISTRO "$distro"
v PRIV "$priv"
v KERNEL "$(uname -r)"
