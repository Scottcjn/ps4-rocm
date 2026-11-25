#!/usr/bin/env bash
# setup-wifi.sh — join a WPA2 network on the PS4's built-in Marvell wifi (mwifiex_sdio).
# Runs ON the PS4. Password comes from the environment, never from this repo.
#
#   usage: WIFI_SSID='MyNetwork' WIFI_PASS='...' SUDO_PASS='...' ./setup-wifi.sh
#
# iwd for the radio + systemd-networkd for DHCP (networkd already runs the ethernet on PS4 Arch).
# Wifi gets RouteMetric 600 (< ethernet's 1024) so internet goes out the wifi; the 10.42.0.x
# ethernet link to the head host stays up for ssh.
# Gotchas (2026-11-25, thebe):
#   - iwd DELETES the driver's interface (mlan0) and recreates it as wlanN -> match wlan* in networkd
#   - `iwctl station X connect` blocks on an interactive passphrase prompt over ssh -> use --passphrase
set -euo pipefail
: "${WIFI_SSID:?set WIFI_SSID}" "${WIFI_PASS:?set WIFI_PASS}"
HERE="$(cd "$(dirname "$0")" && pwd)"
eval "$(bash "$HERE/detect.sh")"
. "$HERE/lib.sh"
setup_sudo

grep -qs mwifiex /sys/class/net/*/device/uevent /sys/class/ieee80211/*/device/uevent || lsmod | grep -q mwifiex \
  || { echo "ERROR: no Marvell wifi (mwifiex) found — check: dmesg | grep mwifiex"; exit 1; }

case "$PKG" in
  pacman) sudo_ pacman -S --noconfirm --needed iwd ;;
  apt)    sudo_ apt-get install -y iwd ;;
esac

# iwd network profile (root-only)
sudo_ mkdir -p /var/lib/iwd
printf '[Security]\nPassphrase=%s\n' "$WIFI_PASS" | write_root "/var/lib/iwd/$WIFI_SSID.psk" 600

# Address on whatever wlanN iwd creates: DHCP by default, or pinned with
#   WIFI_STATIC=192.168.1.50/24 WIFI_GW=192.168.1.1 WIFI_DNS=192.168.1.1
# (pinned on the node because we have no router login; a router reservation for the
#  wifi MAC is still the belt-and-braces fix against the router leasing the same IP elsewhere)
if [ -n "${WIFI_STATIC:-}" ]; then
  : "${WIFI_GW:?WIFI_STATIC needs WIFI_GW}"
  printf '[Match]\nName=wlan*\n\n[Network]\nAddress=%s\nDNS=%s\nIgnoreCarrierLoss=3s\n\n[Route]\nGateway=%s\nMetric=600\n' \
    "$WIFI_STATIC" "${WIFI_DNS:-$WIFI_GW}" "$WIFI_GW" | write_root /etc/systemd/network/30-wifi.network 644
else
  printf '[Match]\nName=wlan*\n\n[Network]\nDHCP=yes\nIgnoreCarrierLoss=3s\n\n[DHCPv4]\nRouteMetric=600\n' | \
    write_root /etc/systemd/network/30-wifi.network 644
fi

sudo_ systemctl enable --now iwd
sudo_ systemctl restart systemd-networkd
sleep 4
IFACE="$(iwctl device list 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '$NF=="station"{print $1; exit}')"
[ -n "$IFACE" ] || { echo "ERROR: iwd sees no station device — journalctl -u iwd -b | tail"; exit 1; }
echo "wifi interface: $IFACE"

iwctl station "$IFACE" scan || true; sleep 5
timeout 30 iwctl --passphrase "$WIFI_PASS" station "$IFACE" connect "$WIFI_SSID" || true

for i in $(seq 1 20); do
  addr="$(ip -4 -br addr show "$IFACE" | awk '{print $3}')"
  [ -n "$addr" ] && { echo "connected: $IFACE $addr"; ip route | grep default; exit 0; }
  sleep 2
done
echo "not connected yet — check: iwctl station $IFACE show ; journalctl -u iwd -b | tail"
exit 1
