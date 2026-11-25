#!/usr/bin/env bash
# ps4-ftp-kernel.sh — swap the PS4 Linux kernel over GoldHEN's FTP server (PS4 booted into its normal OS).
#
#   usage: ps4-ftp-kernel.sh <ps4-ip> status
#          ps4-ftp-kernel.sh <ps4-ip> install  <new-bzImage> <stock-bzImage-backup>
#          ps4-ftp-kernel.sh <ps4-ip> rollback
#
# The PS4 Linux payload always loads a file named exactly `bzImage` (it checks /mnt/usb0, /mnt/usb1, then
# /data/linux/boot), so testing a new kernel over FTP means swapping names:
#   install : refuses unless the current bzImage matches <stock-bzImage-backup>; uploads the new kernel as
#             bzImage-new, re-downloads it and compares sha256, then renames bzImage -> bzImage-stock and
#             bzImage-new -> bzImage.
#   rollback: bzImage -> bzImage-failed, bzImage-stock -> bzImage.
# The PS4's own OS is never touched, so a kernel that won't boot is recovered by booting the PS4 normally
# and running `rollback`.
set -euo pipefail
IP="$1" CMD="$2"
PORT="${PS4_FTP_PORT:-2121}"
DIR="/data/linux/boot"
URL="ftp://$IP:$PORT$DIR"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

ls_dir()  { curl -sS --max-time 20 --list-only "$URL/"; }
sha_rem() { curl -sS --max-time 300 -o "$TMP/r" "$URL/$1" && sha256sum "$TMP/r" | cut -d' ' -f1; }
rename()  { curl -sS --max-time 20 -o /dev/null -Q "RNFR $DIR/$1" -Q "RNTO $DIR/$2" "$URL/"; }

case "$CMD" in
  status)
    echo "== $DIR on $IP:$PORT"; ls_dir
    for f in bzImage bzImage-stock; do
      ls_dir | grep -qx "$f" && echo "$f sha256 $(sha_rem "$f")"
    done ;;
  install)
    NEW="$3" STOCK="$4"
    want_stock="$(sha256sum "$STOCK" | cut -d' ' -f1)"; want_new="$(sha256sum "$NEW" | cut -d' ' -f1)"
    ls_dir | grep -qx bzImage-stock && { echo "ERROR: bzImage-stock already exists — run rollback or inspect first"; exit 1; }
    cur="$(sha_rem bzImage)"
    [ "$cur" = "$want_stock" ] || { echo "ERROR: current bzImage ($cur) != stock backup ($want_stock); refusing"; exit 1; }
    echo "current bzImage matches stock backup"
    curl -sS --max-time 600 -T "$NEW" "$URL/bzImage-new"
    got="$(sha_rem bzImage-new)"
    [ "$got" = "$want_new" ] || { echo "ERROR: upload verify failed ($got != $want_new); bzImage untouched"; exit 1; }
    echo "upload verified ($got)"
    rename bzImage bzImage-stock
    rename bzImage-new bzImage
    [ "$(sha_rem bzImage)" = "$want_new" ] && [ "$(sha_rem bzImage-stock)" = "$want_stock" ] \
      && echo "INSTALLED: bzImage = new kernel, bzImage-stock = original" \
      || { echo "ERROR: post-swap check failed — run status"; exit 1; } ;;
  rollback)
    ls_dir | grep -qx bzImage-stock || { echo "ERROR: no bzImage-stock to roll back to"; exit 1; }
    ls_dir | grep -qx bzImage && rename bzImage bzImage-failed
    rename bzImage-stock bzImage
    echo "ROLLED BACK: bzImage = original ($(sha_rem bzImage))" ;;
  *) echo "unknown command $CMD"; exit 2 ;;
esac
