#!/usr/bin/env bash
# add-node.sh — run on the host for EACH new PS4 after it's cabled in and has an address.
#
#   usage: ./add-node.sh <name> <mac> <ip> [user]
#   e.g.   ./add-node.sh thebe aa:bb:cc:dd:ee:ff 10.42.0.85 ps4
#
# Does: pins the DHCP address to the MAC, adds an ssh alias, installs the host ssh key,
#       pushes this repo to the node and runs node/install-node.sh there.
# Password for the node user is read from $NODE_PASS (never hard-coded in the repo).
set -euo pipefail
NAME="$1" MAC="$2" IP="$3" USER_="${4:-ps4}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
: "${NODE_PASS:?set NODE_PASS to the node user password}"

# 1. DHCP reservation (NetworkManager's shared-mode dnsmasq reads this dir)
echo "dhcp-host=$MAC,$IP,$NAME" | sudo tee "/etc/NetworkManager/dnsmasq-shared.d/$NAME-ps4.conf" >/dev/null
nmcli con up ics-share >/dev/null

# 2. ssh alias
if ! grep -qE "^Host .*\b$NAME\b" ~/.ssh/config 2>/dev/null; then
  printf '\nHost %s\n    HostName %s\n    User %s\n    IdentityFile ~/.ssh/id_ed25519\n    IdentitiesOnly yes\n' \
    "$NAME" "$IP" "$USER_" >> ~/.ssh/config
fi

# 3. key install (password auth forced: too many agent keys -> "Too many authentication failures")
until ping -c1 -W2 "$IP" >/dev/null; do echo "waiting for $IP..."; sleep 3; done
SSHPASS="$NODE_PASS" sshpass -e ssh-copy-id -o PreferredAuthentications=password \
  -o PubkeyAuthentication=no -i ~/.ssh/id_ed25519.pub "$USER_@$IP"

# 4. push repo + run node installer
mkdir -p "$REPO/results"
# tar-over-ssh, not rsync: a fresh PS4 image has no rsync until install-node.sh runs
tar -C "$REPO" --exclude .git --exclude results -cf - . | ssh "$NAME" 'mkdir -p ps4-ai-cluster && tar -C ps4-ai-cluster -xf -'
ssh "$NAME" "SUDO_PASS='$NODE_PASS' bash ps4-ai-cluster/node/install-node.sh" | tee "$REPO/results/$NAME-install.log"
