# lib.sh — shared helpers for node scripts. Source it:  . "$HERE/lib.sh"
#
# setup_sudo: defines sudo_() for root / sudo users.
#   With SUDO_PASS set, uses an ASKPASS helper (mode 700, deleted on exit), because on thebe:
#     - `echo pass | sudo -S cmd` steals stdin from `printf x | sudo_ tee file` -> EMPTY files
#     - `sudo -S -v` then plain sudo fails inside pipelines (cached creds don't reach the
#       pipeline's subshells over a tty-less ssh) -> "a terminal is required"
#   askpass touches neither stdin nor the timestamp cache, so it works everywhere.

log() { printf '\n=== %s\n' "$*"; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

setup_sudo() {
  case "$PRIV" in
    root) sudo_() { "$@"; } ;;
    sudo)
      if [ -n "${SUDO_PASS:-}" ]; then
        _ASKPASS="$(mktemp "${TMPDIR:-/tmp}/askpass.XXXXXX")"
        chmod 700 "$_ASKPASS"
        printf '#!/bin/sh\nprintf "%%s\\n" %q\n' "$SUDO_PASS" > "$_ASKPASS"
        trap 'rm -f "$_ASKPASS"' EXIT
        export SUDO_ASKPASS="$_ASKPASS"
        sudo_() { sudo -A "$@"; }
      else
        sudo_() { sudo "$@"; }          # interactive: prompts on the terminal
      fi
      sudo_ true || die "sudo failed — wrong SUDO_PASS?" ;;
    *) die "user $(id -un) is not root and not in wheel/sudo — fix: su -c 'usermod -aG wheel $(id -un)'" ;;
  esac
}

# write_root <file> <mode>  — content from stdin; verifies the file is non-empty afterwards
write_root() {
  sudo_ tee "$1" >/dev/null
  sudo_ chmod "$2" "$1"
  [ "$(sudo_ stat -c %s "$1")" -gt 0 ] || die "$1 came out empty"
}
