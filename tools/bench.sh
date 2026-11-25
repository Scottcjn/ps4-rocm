#!/usr/bin/env bash
# Run llama-bench remotely and append normalized JSONL results locally.
set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "usage: $0 <node> <model-path> [ngl]" >&2
  exit 2
fi

NODE=$1
MODEL=$2
NGL=${3:-99}
BENCH_TIMEOUT=${BENCH_TIMEOUT:-600}
case "$NGL" in ''|*[!0-9]*) echo "ngl must be a non-negative integer" >&2; exit 2;; esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RESULTS="$ROOT/results/bench.jsonl"
TMP_DIR=$(mktemp -d)
RAW="$TMP_DIR/raw"
ERR="$TMP_DIR/err"
trap 'rm -rf "$TMP_DIR"' EXIT
mkdir -p "$ROOT/results"

set +e
model_b64=$(printf '%s' "$MODEL" | base64 | tr -d '\n')
timeout --signal=TERM --kill-after=10 "$BENCH_TIMEOUT" \
  ssh -o BatchMode=yes -- "$NODE" "MODEL_B64=$model_b64 NGL=$NGL sh -s" >"$RAW" 2>"$ERR" <<'REMOTE'
set -eu
model=$(printf '%s' "$MODEL_B64" | base64 -d)
ngl=$NGL
case "$model" in '~/'*) model="$HOME/${model#\~/}";; esac
bin="$HOME/llama.cpp/build-vk/bin/llama-bench"
field() { sed -n "s/^$1=//p" "$HOME/node-info.txt" 2>/dev/null | head -1 | tr '\t\r\n' '   '; }
printf '__META__\t%s\t%s\t%s\n' "$(field PS4_MODEL)" \
  "$(git -C "$HOME/llama.cpp" rev-parse --short HEAD 2>/dev/null || echo unknown)" "$(field MESA)"
echo __BENCH__
if "$bin" --help 2>&1 | grep -Eq -- '(^|[[:space:],])-o([[:space:],]|$).*json|output.*json'; then
  exec "$bin" -m "$model" -ngl "$ngl" -o json
else
  exec "$bin" -m "$model" -ngl "$ngl"
fi
REMOTE
rc=$?
set -e

timestamp=$(date -u +%FT%TZ)
if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
  # A second, short SSH call is intentional: the timed-out connection cannot do cleanup.
  ssh -o BatchMode=yes -o ConnectTimeout=5 -- "$NODE" 'pkill -x llama-bench || true' >/dev/null 2>&1 || true
  line=$(python3 -c 'import json,sys; print(json.dumps({"status":"hang","node":sys.argv[1],"model":sys.argv[2],"ngl":int(sys.argv[3]),"timestamp":sys.argv[4]},separators=(",",":"),sort_keys=True))' "$NODE" "$MODEL" "$NGL" "$timestamp")
  printf '%s\n' "$line" | tee -a "$RESULTS"
  exit 124
fi
if [ "$rc" -ne 0 ]; then
  sed -n '1,20p' "$ERR" >&2
  exit "$rc"
fi

meta_line=$(sed -n 's/^__META__[[:space:]]*//p' "$RAW" | head -1)
ps4_model=$(printf '%s' "$meta_line" | cut -f1)
llama_commit=$(printf '%s' "$meta_line" | cut -f2)
mesa=$(printf '%s' "$meta_line" | cut -f3-)
sed -n '/^__BENCH__$/,$p' "$RAW" | sed '1d' >"$TMP_DIR/output"
meta=$(python3 -c 'import json,sys; print(json.dumps({"status":"ok","node":sys.argv[1],"PS4_MODEL":sys.argv[2] or "unknown","llama_cpp_commit":sys.argv[3] or "unknown","mesa":sys.argv[4] or "unknown","model":sys.argv[5],"ngl":int(sys.argv[6]),"timestamp":sys.argv[7]},separators=(",",":")))' \
  "$NODE" "$ps4_model" "$llama_commit" "$mesa" "$MODEL" "$NGL" "$timestamp")

parsed=$(python3 "$ROOT/head/benchparse.py" --meta "$meta" <"$TMP_DIR/output")
if [ -z "$parsed" ]; then
  echo "could not parse llama-bench output" >&2
  exit 1
fi
printf '%s\n' "$parsed" | tee -a "$RESULTS"
