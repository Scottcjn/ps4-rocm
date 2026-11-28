#!/bin/bash
# q4w-run.sh — run ON thebe, only in a GPU window agreed with the other thebe session.
# 1) CPU: copy ~/llama.cpp-rm (stock shaders + GGML_VK_RM_* overrides) to ~/llama.cpp-q4w,
#    drop in the wide-load Q4_K matvec shader, build llama-bench + llama-completion.
# 2) GPU: A/B Qwen3-4B Q4_K_M stock vs wide (tg64, -fa 0, -r 3) at RM_KQ 4 (default), 2, 8,
#    plus greedy-text comparison; then the Q4_0 files (stock trees) for the format comparison.
# Expects ~/benchlogs/q4w/mul_mat_vec_q4_k.comp (the wide shader) to be uploaded first.
set -u
O=~/benchlogs/q4w; L=$O/run.log; mkdir -p $O
log(){ echo "$(date -u +%FT%TZ) $*" | tee -a $L; }
busy(){ pgrep -x llama-server >/dev/null || pgrep -x llama-bench >/dev/null || pgrep -x llama-cli >/dev/null || pgrep -f "bin/llama-completion" >/dev/null || pgrep -f pm4bridge >/dev/null; }
hangs(){ journalctl -k -b --no-pager | grep -ciE "ring .*timeout|preemption time out"; }

# ---------- build (CPU only) ----------
SRC=~/llama.cpp-rm; DST=~/llama.cpp-q4w
[ -x $SRC/build-vk/bin/llama-bench ] || { log "ABORT: $SRC not built"; exit 1; }
rsync -a --delete --exclude '/build*' $SRC/ $DST/ || { log "rsync failed"; exit 1; }
cp $O/mul_mat_vec_q4_k.comp $DST/ggml/src/ggml-vulkan/vulkan-shaders/mul_mat_vec_q4_k.comp
grep -q "wide load" $DST/ggml/src/ggml-vulkan/vulkan-shaders/mul_mat_vec_q4_k.comp || { log "ABORT: wide shader not in place"; exit 1; }
opts=$(grep -E '^(GGML|LLAMA)_[A-Z0-9_]+:BOOL=(ON|OFF)' $SRC/build-vk/CMakeCache.txt | grep -vE 'LLAMA_BUILD_(TESTS|EXAMPLES|SERVER)' | sed -E 's/^([A-Z0-9_]+):BOOL=(.*)$/-D\1=\2/' | tr '\n' ' ')
cmake -S $DST -B $DST/build-vk -DCMAKE_BUILD_TYPE=Release $opts -DLLAMA_BUILD_TESTS=OFF > $O/build.log 2>&1 || { log "cmake failed"; exit 1; }
nice -n 15 cmake --build $DST/build-vk --target llama-bench llama-completion -j4 >> $O/build.log 2>&1
log "build rc=$?"
[ -x $DST/build-vk/bin/llama-bench ] && [ -x $DST/build-vk/bin/llama-completion ] || { log "ABORT: binaries missing"; exit 1; }

[ "${BUILD_ONLY:-0}" = 1 ] && { log "BUILDDONE (BUILD_ONLY, no GPU use)"; exit 0; }

# ---------- GPU ----------
if busy; then log "BUSY: another GPU job is running, not starting"; exit 3; fi
export VK_ICD_FILENAMES=$HOME/mesa-ps4/share/vulkan/icd.d/radeon_icd.x86_64.json GGML_VK_PREFER_HOST_MEMORY=1
h0=$(hangs); log "== GPU start hang0=$h0"
PROMPT="The three primary colors of light are"
M=~/models
bench(){ # tree tag model envspec
  local B=~/llama.cpp-$1/build-vk/bin tag=$2 model=$3 ev=$4
  [ "$(hangs)" = "$h0" ] || { log "ABORT: new GPU hang seen"; return 9; }
  env $ev timeout 600 $B/llama-bench -m $model -ngl 99 -fa 0 -p 0 -n 64 -r 3 -o csv > $O/$tag.csv 2> $O/$tag.err
  log "$tag rc=$? tg64=$(tail -1 $O/$tag.csv | awk -F, '{print $(NF-1)}')"
  env $ev timeout 300 $B/llama-completion -m $model -ngl 99 -fa off -c 512 -no-cnv --temp 0 --seed 1 -n 32 -p "$PROMPT" 2>/dev/null </dev/null > $O/$tag.txt
  sleep 3
}
Q4K=$M/Qwen__Qwen3-4B-GGUF/Qwen3-4B-Q4_K_M.gguf
cat $Q4K > /dev/null
for kq in 4 2 8; do
  bench rm  stock.kq$kq $Q4K GGML_VK_RM_KQ=$kq || break
  bench q4w wide.kq$kq  $Q4K GGML_VK_RM_KQ=$kq || break
  if cmp -s $O/stock.kq$kq.txt $O/wide.kq$kq.txt; then log "kq$kq text MATCH"; else log "kq$kq text DIFFERS (expected possible: different float sum order) -> diff $O/stock.kq$kq.txt $O/wide.kq$kq.txt"; fi
done
python3 -c "import os;fd=os.open('$Q4K',os.O_RDONLY);os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)"

# Q4_0 format comparison on stock trees (skip any file not downloaded yet)
for r in "rm|$M/unsloth__Qwen3-0.6B-GGUF/Qwen3-0.6B-Q4_0.gguf" "rm|$M/unsloth__Qwen3-4B-GGUF/Qwen3-4B-Q4_0.gguf" "r2rm|$M/unsloth__gemma-4-E2B-it-GGUF/gemma-4-E2B-it-Q4_0.gguf" "rm|$(find $M -name Qwen3.5-4B-Q4_0.gguf -print -quit)"; do
  IFS='|' read -r tree model <<< "$r"
  [ -f "$model" ] || { log "MISSING $model"; continue; }
  cat "$model" > /dev/null
  bench $tree q4_0.$(basename $model .gguf) "$model" GGML_VK_RM_STDQ=2 || break
  python3 -c "import os;fd=os.open('$model',os.O_RDONLY);os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)"
done
log "== Q4WDONE hang $h0->$(hangs)"
