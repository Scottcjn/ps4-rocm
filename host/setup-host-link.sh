#!/usr/bin/env bash
# setup-host-link.sh — run ONCE on the host (Victus) to share its network to PS4 nodes over ethernet.
#
#   usage: ./setup-host-link.sh [iface]        (default iface: eno1)
#
# Result: host = 10.42.0.1 on $IFACE, NetworkManager "shared" mode (dnsmasq DHCP + NAT out the wifi).
# Lessons baked in (2026-11-25):
#   - the "mango-lan" DHCP-client profile just hangs on a direct cable; use a shared profile instead
#   - ufw default-drops input, so DHCP requests from the PS4 were silently eaten until eno1 was allowed
set -euo pipefail
IFACE="${1:-eno1}"
CON="ics-share"
WAN="$(ip route show default | awk '{print $5; exit}')"

if ! nmcli -t -f NAME con show | grep -qx "$CON"; then
  nmcli con add type ethernet ifname "$IFACE" con-name "$CON" ipv4.method shared \
        ipv4.addresses 10.42.0.1/24 ipv6.method disabled connection.autoconnect no
fi
nmcli con up "$CON"

sudo ufw allow in on "$IFACE" comment 'PS4 direct link'
sudo ufw route allow in on "$IFACE" out on "$WAN" comment 'PS4 internet via host'

echo "host link up: $IFACE = 10.42.0.1, NAT via $WAN"
echo "to undo: nmcli con up mango-lan ; sudo ufw status numbered  (delete the 'PS4' rules)"
