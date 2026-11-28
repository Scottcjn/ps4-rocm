#!/bin/bash
# rm-run.sh — build both patched trees, then sweep only if all binaries exist.
cd ~/benchlogs/rm || exit 1
./rm-build.sh
for b in ~/llama.cpp-rm ~/llama.cpp-r2rm; do
  for t in llama-bench llama-completion; do
    [ -x "$b/build-vk/bin/$t" ] || { echo "== RMDONE (aborted: missing $b/build-vk/bin/$t)" | tee -a ~/benchlogs/rm/sweep.log; exit 1; }
  done
done
./rm-sweep.sh
