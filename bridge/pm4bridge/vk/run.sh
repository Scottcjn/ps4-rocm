#!/bin/bash
# usage: vk/run.sh <vkbw args...>   - one vkbw run under timeout, with the
# patched-Mesa ICD forced and the kernel trouble count before/after.
cd "$(dirname "$0")" || exit 2
export VK_ICD_FILENAMES=$HOME/mesa-ps4/share/vulkan/icd.d/radeon_icd.x86_64.json
C(){ journalctl -k -b --no-pager | grep -ciE "ring .*timeout|preemption time out|Failed to evict|kfd.*(hang|reset)"; }
L=../logs/vkbw.log
B=$(C)
echo "== $(date -Is) vkbw $*" >> "$L"
timeout -k 5 "${TO:-120}" ./vkbw "$@" >> "$L" 2>&1; RC=$?
A=$(C)
echo "== vkbw $* rc=$RC (124=timeout, 2=hang) trouble-count before=$B after=$A" | tee -a "$L"
tac "$L" | awk "/^== 20/{exit} {print}" | tac | grep -E "SUMMARY|RESULT|buffer|^\[mem|drm-memory-vram|evicted|overhead"
pgrep -l -x vkbw && echo "!! vkbw still alive"
[ "$A" != "$B" ] && { echo "!! NEW KERNEL TROUBLE"; exit 99; }
exit $RC
