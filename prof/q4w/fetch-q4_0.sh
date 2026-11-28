#!/bin/bash
# fetch-q4_0.sh — run ON thebe (CPU/network only, no GPU). Downloads Q4_0 GGUFs of the
# micro models already profiled, each with its card + licence + LICENSE_RECEIPT.txt
# (lab rule 2026-10-16). Anonymous download: no HF account/token is used.
# Pinned revisions so the receipt names exactly what was fetched.
set -u
M=~/models; LOG=~/benchlogs/q4_0-fetch.log; mkdir -p ~/benchlogs; : > $LOG
log(){ echo "$(date -u +%FT%TZ) $*" | tee -a $LOG; }
free_gb=$(df -BG --output=avail $M | tail -1 | tr -dc 0-9)
log "free on $M: ${free_gb} GB"
[ "$free_gb" -ge 15 ] || { log "ABORT: need >=15 GB free"; exit 2; }

HF=https://huggingface.co
# dir | repo | revision | gguf | upstream repo | upstream revision | upstream licence file (or -)
JOBS=(
 "unsloth__Qwen3-0.6B-GGUF|unsloth/Qwen3-0.6B-GGUF|50968a4468ef4233ed78cd7c3de230dd1d61a56b|Qwen3-0.6B-Q4_0.gguf|Qwen/Qwen3-0.6B|c1899de289a04d12100db370d81485cdf75e47ca|LICENSE"
 "unsloth__Qwen3-4B-GGUF|unsloth/Qwen3-4B-GGUF|22c9fc8a8c7700b76a1789366280a6a5a1ad1120|Qwen3-4B-Q4_0.gguf|Qwen/Qwen3-4B|1cfa9a7208912126459214e8b04321603b3df60c|LICENSE"
 "unsloth__gemma-4-E2B-it-GGUF|unsloth/gemma-4-E2B-it-GGUF|0314792d7f1f7e229411f620751375812bb9faf2|gemma-4-E2B-it-Q4_0.gguf|google/gemma-4-E2B-it|4adf15748de9e1a0097b6274cee9c15836c41c37|-"
)
for jb in "${JOBS[@]}"; do
  IFS='|' read -r dir repo rev gguf up uprev uplic <<< "$jb"
  D=$M/$dir; mkdir -p "$D"; cd "$D" || continue
  log "== $repo@$rev $gguf"
  # expected sha256 = LFS oid from the pinned tree
  want=$(curl -s -A hf-client "$HF/api/models/$repo/tree/$rev" | python3 -c "
import sys,json
for f in json.load(sys.stdin):
    if f['path']=='$gguf': print((f.get('lfs') or {}).get('oid',''))")
  [ -n "$want" ] || { log "no LFS oid for $gguf, skipping"; continue; }
  if [ -f "$gguf" ] && [ "$(sha256sum "$gguf" | cut -d' ' -f1)" = "$want" ]; then log "already present, sha ok"
  else
    nice -n 15 ionice -c3 curl -sSfL -A hf-client -C - -o "$gguf.part" "$HF/$repo/resolve/$rev/$gguf" || { log "download failed rc=$?"; continue; }
    got=$(sha256sum "$gguf.part" | cut -d' ' -f1)
    [ "$got" = "$want" ] || { log "SHA MISMATCH got=$got want=$want (kept .part)"; continue; }
    mv "$gguf.part" "$gguf"; log "sha256 ok $got"
  fi
  # cards + licence (quantizer card, upstream card, upstream licence if it has one)
  curl -sSfL -A hf-client -o "README.md.$rev" "$HF/$repo/resolve/$rev/README.md"
  curl -sSfL -A hf-client -o "UPSTREAM_README.md.$uprev" "$HF/$up/resolve/$uprev/README.md"
  [ "$uplic" != "-" ] && curl -sSfL -A hf-client -o "UPSTREAM_LICENSE.$uprev" "$HF/$up/resolve/$uprev/$uplic"
  lic_card=$(sed -n '1,30p' "README.md.$rev" | grep -m1 -i '^license:' | tr -d '\r')
  lic_up=$(sed -n '1,40p' "UPSTREAM_README.md.$uprev" | grep -m1 -i '^license:' | tr -d '\r')
  {
    echo ""
    echo "===== Q4_0 receipt $(date -u +%FT%TZ) ====="
    echo "file:            $gguf  sha256=$want"
    echo "repo:            $repo  revision=$rev"
    echo "upstream model:  $up  revision=$uprev"
    echo "download date:   $(date -u +%F) (UTC)"
    echo "account used:    none (anonymous HTTPS download, no Hugging Face login or token)"
    echo "licence:         Apache-2.0 per model-card metadata (quantizer card '$lic_card'; upstream card '$lic_up')"
    echo "gated:           no (no licence acceptance was required or given)"
    echo "licence files (sha256):"
    for f in "README.md.$rev" "UPSTREAM_README.md.$uprev" UPSTREAM_LICENSE.*; do [ -f "$f" ] && echo "  $(sha256sum "$f")"; done
    [ "$uplic" = "-" ] && echo "  note: neither $repo nor $up ships a LICENSE file at these revisions; the licence is stated only in the model cards above."
    echo "terms relied on: Apache License 2.0 — commercial use allowed, no revenue/user threshold;"
    echo "                 outputs are not restricted by the licence; redistribution of the weights must"
    echo "                 keep the licence + any NOTICE and mark modifications; no trademark rights granted."
    echo "use here:        local benchmarking on PS4 Pro (thebe); weights not redistributed."
  } >> LICENSE_RECEIPT.txt
  log "receipt written: $D/LICENSE_RECEIPT.txt"
  python3 -c "import os;fd=os.open('$D/$gguf',os.O_RDONLY);os.posix_fadvise(fd,0,0,os.POSIX_FADV_DONTNEED)" 2>/dev/null
done
log "FETCHDONE"
