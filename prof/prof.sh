#!/bin/bash
# prof.sh <tag> <model.gguf>   — run on thebe; one GPU job at a time.
# 1) plain tg run for wall-clock ms/token  2) same with GGML_VK_PERF_LOGGER for GPU time per op.
set -u
tag=$1; m=$2; O=~/benchlogs/prof; mkdir -p $O
B=${B:-~/llama.cpp/build-vk/bin/llama-bench}
export VK_ICD_FILENAMES=$HOME/mesa-ps4/share/vulkan/icd.d/radeon_icd.x86_64.json GGML_VK_PREFER_HOST_MEMORY=1
busy(){ pgrep -x llama-server >/dev/null || pgrep -x llama-bench >/dev/null || pgrep -x llama-cli >/dev/null || pgrep -f pm4bridge >/dev/null; }
if busy; then echo "BUSY: another GPU job is running, not starting"; exit 3; fi
hang0=$(journalctl -k -b --no-pager | grep -ciE "ring .*timeout|preemption time out")
timeout 600 $B -m $m -ngl 99 -p 0 -n 64 -r 3 -o csv > $O/$tag.plain.csv 2> $O/$tag.plain.err; echo "plain rc=$?"
sleep 3
GGML_VK_PERF_LOGGER=1 timeout 600 $B -m $m -ngl 99 -p 0 -n 32 -r 1 -o csv > $O/$tag.perf.csv 2> $O/$tag.perf.err; echo "perf rc=$?"
hang1=$(journalctl -k -b --no-pager | grep -ciE "ring .*timeout|preemption time out")
echo "hang $hang0->$hang1"
