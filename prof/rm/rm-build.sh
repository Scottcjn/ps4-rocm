#!/bin/bash
# rm-build.sh — run on thebe. Copies ~/llama.cpp and ~/llama.cpp-r2 into NEW trees,
# inserts the GGML_VK_RM_* override block, builds llama-bench + llama-completion.
# Never touches the original trees. CPU-only (no GPU use).
set -u
cd ~ || exit 1
BLOCK=~/benchlogs/rm/block.inc
LOG=~/benchlogs/rm/build.log
mkdir -p ~/benchlogs/rm
: > $LOG
build_one() {
  local src=$1 dst=$2
  echo "== $src -> $dst" | tee -a $LOG
  rsync -a --delete --exclude '/build*' "$src/" "$dst/" || { echo "rsync failed" | tee -a $LOG; return 1; }
  python3 - "$dst/ggml/src/ggml-vulkan/ggml-vulkan.cpp" "$BLOCK" <<'PY' 2>&1 | tee -a $LOG
import sys
path, blk = sys.argv[1], open(sys.argv[2]).read()
src = open(path).read()
anchor = "    uint32_t rm_iq = 2 * rm_kq;\n"
if "GGML_VK_RM_KQ" in src:
    print("already patched"); sys.exit(0)
if src.count(anchor) != 1:
    print("ANCHOR NOT FOUND (count=%d) - not patching" % src.count(anchor)); sys.exit(2)
open(path, "w").write(src.replace(anchor, anchor + blk, 1))
print("patched", path)
PY
  [ "${PIPESTATUS[0]}" = 0 ] || return 1
  # reuse the ON options of the original Vulkan build
  local opts
  opts=$(grep -E '^(GGML|LLAMA)_[A-Z0-9_]+:BOOL=(ON|OFF)' "$src/build-vk/CMakeCache.txt" | grep -vE 'LLAMA_BUILD_(TESTS|EXAMPLES|SERVER)' | sed -E 's/^([A-Z0-9_]+):BOOL=(.*)$/-D\1=\2/' | tr '\n' ' ')
  echo "opts: $opts" >> $LOG
  cmake -S "$dst" -B "$dst/build-vk" -DCMAKE_BUILD_TYPE=Release $opts -DLLAMA_BUILD_TESTS=OFF >> $LOG 2>&1 || { echo "cmake failed" | tee -a $LOG; return 1; }
  nice -n 15 cmake --build "$dst/build-vk" --target llama-bench llama-completion -j4 >> $LOG 2>&1
  echo "build rc=$? $dst" | tee -a $LOG
  ls -la "$dst/build-vk/bin/llama-bench" "$dst/build-vk/bin/llama-completion" 2>&1 | tee -a $LOG
}
build_one ~/llama.cpp    ~/llama.cpp-rm
build_one ~/llama.cpp-r2 ~/llama.cpp-r2rm
grep -n "GGML_TYPE_PQ2_0\]\[i\]" ~/llama.cpp-r2rm/ggml/src/ggml-vulkan/ggml-vulkan.cpp | head -3 | tee -a $LOG
echo "BUILDDONE" | tee -a $LOG
