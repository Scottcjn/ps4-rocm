#!/usr/bin/env bash
# fetch-model.sh — download one GGUF from Hugging Face WITH its license + card + LICENSE_RECEIPT.txt
# (lab rule since 2026-10-16: every model keeps its license, pinned to the exact revision).
#
#   usage: fetch-model.sh <hf-repo> <file.gguf> [models-dir]
#   e.g.   fetch-model.sh Qwen/Qwen3-0.6B-GGUF Qwen3-0.6B-Q8_0.gguf
#
# Only for ungated repos. Gated licenses are accepted by a human, never by a script.
set -euo pipefail
REPO="$1" FILE="$2" DIR="${3:-$HOME/models}"
DEST="$DIR/${REPO//\//__}"
mkdir -p "$DEST"

api="https://huggingface.co/api/models/$REPO"
meta="$(curl -fsSL "$api")"
SHA="$(printf '%s' "$meta" | python3 -c 'import json,sys; print(json.load(sys.stdin)["sha"])')"
LIC="$(printf '%s' "$meta" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("cardData",{}).get("license") or "unknown")')"
GATED="$(printf '%s' "$meta" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("gated", False))')"
[ "$GATED" = False ] || { echo "ERROR: $REPO is gated ($GATED) — a human must accept the license on huggingface.co first"; exit 1; }

base="https://huggingface.co/$REPO/resolve/$SHA"
[ -s "$DEST/$FILE" ] || curl -fL --retry 3 -C - -o "$DEST/$FILE.part" "$base/$FILE" && { [ -f "$DEST/$FILE.part" ] && mv "$DEST/$FILE.part" "$DEST/$FILE"; true; }

# license / notice / card — whichever exist in this revision
files="$(printf '%s' "$meta" | python3 -c 'import json,sys; print("\n".join(s["rfilename"] for s in json.load(sys.stdin).get("siblings",[])))')"
kept=()
for f in $(printf '%s\n' "$files" | grep -iE '^(LICENSE|NOTICE|README\.md)' || true); do
  curl -fsSL -o "$DEST/$f" "$base/$f" && kept+=("$f")
done
[ ${#kept[@]} -gt 0 ] || echo "WARN: no LICENSE/NOTICE/README in $REPO@$SHA — license is only the card tag ($LIC)"

{
  echo "repo:            $REPO"
  echo "revision_sha:    $SHA"
  echo "download_utc:    $(date -u +%FT%TZ)"
  echo "account:         anonymous (public, ungated)"
  echo "license:         $LIC"
  echo "downloaded_on:   $(cat /etc/hostname 2>/dev/null)"
  echo "weights:"
  echo "  $FILE  sha256=$(sha256sum "$DEST/$FILE" | cut -d' ' -f1)"
  echo "license_files:"
  for f in "${kept[@]}"; do echo "  $f  sha256=$(sha256sum "$DEST/$f" | cut -d' ' -f1)"; done
  echo "terms_relied_on: see LICENSE/README in this folder (fill in commercial/attribution notes when shipping outputs)"
} > "$DEST/LICENSE_RECEIPT.txt"
echo "$DEST/$FILE"
