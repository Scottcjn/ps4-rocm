#!/bin/bash
# rm-sweep.sh — run on thebe AFTER rm-build.sh, only in a GPU window agreed with the
# other thebe session. One GPU job at a time. Writes ~/benchlogs/rm/sweep.log.
# For each model: sweep the row-count env var its quant type uses, tg64 with -fa 0,
# then a greedy 32-token completion per setting; text must match the default exactly
# (rows per workgroup only regroups rows, each row's reduction is unchanged).
set -u
O=~/benchlogs/rm; L=$O/sweep.log; mkdir -p $O
export VK_ICD_FILENAMES=$HOME/mesa-ps4/share/vulkan/icd.d/radeon_icd.x86_64.json GGML_VK_PREFER_HOST_MEMORY=1
busy(){ pgrep -x llama-server >/dev/null || pgrep -x llama-bench >/dev/null || pgrep -x llama-cli >/dev/null || pgrep -f "bin/llama-completion" >/dev/null || pgrep -f pm4bridge >/dev/null; }
if busy; then echo "BUSY: another GPU job is running, not starting" | tee -a $L; exit 3; fi
hangs(){ journalctl -k -b --no-pager | grep -ciE "ring .*timeout|preemption time out"; }
h0=$(hangs); echo "== start $(date -u +%FT%TZ) hang0=$h0" | tee -a $L
PROMPT="The three primary colors of light are"
M=~/models
# Which row variable does PQ2_0 use in the PrismML tree? (rm_kq / rm_iq / rm_stdq)
pqv=$(grep -m1 "GGML_TYPE_PQ2_0\]\[i\]" ~/llama.cpp-r2rm/ggml/src/ggml-vulkan/ggml-vulkan.cpp | grep -oE "\{[0-9]*\*?rm_(kq|iq|stdq)\b" | head -1 | grep -oE "rm_(kq|iq|stdq)")
case "$pqv" in rm_kq) BONSAI_VAR=GGML_VK_RM_KQ;; rm_iq) BONSAI_VAR=GGML_VK_RM_IQ;; rm_stdq) BONSAI_VAR=GGML_VK_RM_STDQ;; *) BONSAI_VAR=SKIP;; esac
echo "PQ2_0 row var: '$pqv' -> $BONSAI_VAR" | tee -a $L
# tree | model | env var | values (first = default for that var on GCN)
RUNS=(
 "rm|$M/Qwen__Qwen3-4B-GGUF/Qwen3-4B-Q4_K_M.gguf|GGML_VK_RM_KQ|4 2 8 16"
 "rm|$M/Qwen__Qwen3-0.6B-GGUF/Qwen3-0.6B-Q8_0.gguf|GGML_VK_RM_STDQ|2 1 4 8"
 "r2rm|$M/unsloth__gemma-4-E2B-it-GGUF/gemma-4-E2B-it-IQ4_XS.gguf|GGML_VK_RM_IQ|8 4 16"
 "r2rm|$M/prism-ml__Ternary-Bonsai-2-2B-gguf/Ternary-Bonsai-2-2B-PQ2_0.gguf|${BONSAI_VAR:-GGML_VK_RM_KQ}|4 2 8 16"
)
for r in "${RUNS[@]}"; do
  IFS='|' read -r tree model var vals <<< "$r"
  B=~/llama.cpp-$tree/build-vk/bin; tag=$(basename $model .gguf)
  [ -f "$model" ] || { echo "MISSING $model" | tee -a $L; continue; }
  [ "$var" = SKIP ] && { echo "SKIP $tag: row var not found" | tee -a $L; continue; }
  cat "$model" > /dev/null   # pre-warm page cache
  ref=""
  for v in $vals; do
    [ "$(hangs)" = "$h0" ] || { echo "ABORT: new GPU hang seen" | tee -a $L; break 2; }
    echo "-- $tag $var=$v" | tee -a $L
    env $var=$v timeout 600 $B/llama-bench -m $model -ngl 99 -fa 0 -p 0 -n 64 -r 3 -o csv > $O/$tag.$v.csv 2> $O/$tag.$v.err
    echo "bench rc=$? $(tail -1 $O/$tag.$v.csv | awk -F, '{print $(NF-1)}')" | tee -a $L
    grep -h "ggml_vulkan: GGML_VK_RM" $O/$tag.$v.err | head -1 | tee -a $L
    out=$(env $var=$v timeout 300 $B/llama-completion -m $model -ngl 99 -fa off -no-cnv --temp 0 --seed 1 -n 32 -p "$PROMPT" 2>/dev/null </dev/null)
    echo "$out" > $O/$tag.$v.txt
    if [ -z "$ref" ]; then ref="$out"; echo "ref text saved" | tee -a $L
    elif [ "$out" = "$ref" ]; then echo "text MATCH" | tee -a $L
    else echo "text DIFFERS from default  <-- check $O/$tag.$v.txt" | tee -a $L; fi
    sleep 3
  done
  python3 -c "import os;fd=os.open('$model',os.O_RDONLY);os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)"
done
echo "== RMDONE $(date -u +%FT%TZ) hang $h0->$(hangs)" | tee -a $L
