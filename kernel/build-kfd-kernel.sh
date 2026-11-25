#!/usr/bin/env bash
# build-kfd-kernel.sh — build the Strawberry PS4 kernel WITH KFD (/dev/kfd, needed for ROCm).
# Runs on the HOST (needs clang/lld, ~10 GB free disk). Reproduces what was done by hand on 2026-11-25.
#
#   usage: ./build-kfd-kernel.sh <workdir> <node-ssh-alias>      e.g. ./build-kfd-kernel.sh ~/elya-work/ps4-kernel thebe
#
# Steps:
#   1. clone Strawberry 7.3.4 (gitlab.com/rmuxnet/linux, branch ps4/7.3.4) — matches the running kernel
#   2. take the node's RUNNING config (/proc/config.gz) + CONFIG_HSA_AMD=y (SVM off: gfx9+ only)
#   3. apply 0001-amdkfd-register-ps4-liverpool-gladius.patch (the only missing KFD piece)
#   4. firmware: build.sh needs blobs it can't download (mrvl/sd8797_uapsta.bin = "useCustomFirmware").
#      We lift ALL built-in blobs from the node's current bzImage (tools/extract-builtin-fw.py).
#      The bzImage comes from the PS4 user partition (/dev/mapper/ps4hdd, UFS2), attached and
#      immediately remounted read-only — that copy is also the rollback backup.
#   5. ./build.sh --option 1 use=Server lto=FullLTO   (option 1 = build only, no firmware fetch)
#
# Firmware blobs are Sony/Marvell/MediaTek proprietary: they stay in <workdir>, never in this repo.
# NOTE: step 4 runs sudo on the node over a non-tty ssh -> the node user needs a cached sudo
#       or NOPASSWD for mount/umount/tar; otherwise do step 4 by hand (see NOTES.md).
set -euo pipefail
WORK="$1" NODE="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$WORK/linux"
mkdir -p "$WORK"

[ -d "$SRC/.git" ] || git clone --depth 1 --single-branch -b ps4/7.3.4 https://gitlab.com/rmuxnet/linux.git "$SRC"

# 2. running config
ssh "$NODE" 'zcat /proc/config.gz' > "$WORK/$NODE-running.config"

# 4. back up the node's boot files + extract firmware
mkdir -p "$WORK/$NODE-boot-backup"
ssh "$NODE" 'sudo mkdir -p /mnt/ps4hdd &&
  { findmnt /mnt/ps4hdd >/dev/null || sudo mount -t ufs -o ufstype=ufs2 /dev/mapper/ps4hdd /mnt/ps4hdd; } &&
  sudo mount -o remount,bind,ro /mnt/ps4hdd &&
  sudo tar -C /mnt/ps4hdd/data/linux/boot -cf - . ; sudo umount /mnt/ps4hdd' | tar -C "$WORK/$NODE-boot-backup" -xf -
( cd "$WORK/$NODE-boot-backup" && sha256sum ./* > SHA256SUMS )
"$SRC/scripts/extract-vmlinux" "$WORK/$NODE-boot-backup/bzImage" > "$WORK/$NODE-vmlinux"
python3 -I "$HERE/../tools/extract-builtin-fw.py" "$WORK/$NODE-vmlinux" "$WORK/$NODE-firmware" > "$WORK/$NODE-firmware.sha256"

# 3. patch (idempotent)
cd "$SRC"
for p in "$HERE"/0*.patch; do
  if git apply --check "$p" 2>/dev/null; then git apply "$p"; echo "applied $(basename "$p")"
  else echo "skip $(basename "$p") (already applied?)"; fi
done

# config + firmware into the tree
[ -f config.strawberry-default ] || cp config config.strawberry-default
cp "$WORK/$NODE-running.config" config
scripts/config --file config --enable CONFIG_HSA_AMD --disable CONFIG_HSA_AMD_SVM
mkdir -p extra_firmware && cp -r "$WORK/$NODE-firmware/." extra_firmware/

# 5. build
# Ubuntu/Debian ship LLVM tools as llvm-objcopy-20 etc.; the unversioned names (which LLVM=1 calls)
# live in /usr/lib/llvm-N/bin. Without this the build fails late with "llvm-objcopy: not found".
if ! command -v llvm-objcopy >/dev/null; then
  LLVM_BIN="$(ls -d /usr/lib/llvm-*/bin 2>/dev/null | sort -V | tail -1)"
  [ -n "$LLVM_BIN" ] && [ -x "$LLVM_BIN/llvm-objcopy" ] || { echo "ERROR: llvm-objcopy not found — install llvm"; exit 1; }
  export PATH="$LLVM_BIN:$PATH"; echo "using LLVM tools from $LLVM_BIN"
fi
./build.sh --option 1 use=Server lto=FullLTO jobs="${JOBS:-12}" 2>&1 | tee "$WORK/build-kfd.log"
ls -la out/bzImage && sha256sum out/bzImage
grep -q 'CONFIG_HSA_AMD=y' .config && echo "KFD enabled in built kernel config"
