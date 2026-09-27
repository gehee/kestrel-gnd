#!/usr/bin/env bash
#
# optimize-boot.sh — cut boot time on kestrel ground station (RK3588)
#
# Safe to re-run. Keeps: ssh, NetworkManager (for AR803x Ethernet),
# systemd-resolved, dbus, udev, rsyslog.
#
# Usage:  ./optimize-boot.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_DST="/etc/systemd/system/kestrel-gnd.service"
BIN="${SCRIPT_DIR}/build/kestrel-gnd"

if [[ ${EUID} -ne 0 ]]; then
    echo "==> Needs root; re-running with sudo..." >&2
    exec sudo -- "$0" "$@"
fi

# ── 1. Remove snapd (lxd + snapd itself) ─────────────────────────────────────
echo "==> Removing snapd and snap packages"
if command -v snap &>/dev/null; then
    # Remove snaps in dependency order
    for pkg in lxd core22 core24 snapd; do
        if snap list "$pkg" &>/dev/null 2>&1; then
            echo "    snap remove $pkg"
            snap remove --purge "$pkg" || true
        fi
    done
fi
if dpkg -l snapd &>/dev/null 2>/dev/null && dpkg -l snapd | grep -q "^ii"; then
    echo "    apt purge snapd"
    DEBIAN_FRONTEND=noninteractive apt-get purge -y snapd
    apt-get autoremove -y --purge
    # Prevent snapd from being reinstalled by apt
    cat > /etc/apt/preferences.d/no-snapd.pref <<'EOF'
Package: snapd
Pin: release *
Pin-Priority: -1
EOF
    echo "    snapd pinned to -1 (won't be reinstalled)"
fi
# Clean up leftover snap mount points
for d in /snap/bin /snap; do
    if mountpoint -q "$d" 2>/dev/null; then umount -l "$d" || true; fi
done

# ── 2. Disable NetworkManager-wait-online ────────────────────────────────────
echo "==> Disabling NetworkManager-wait-online.service (saves ~5s)"
systemctl disable NetworkManager-wait-online.service 2>/dev/null || true
systemctl mask    NetworkManager-wait-online.service

# ── 3. Disable wpa_supplicant (no WiFi on this device) ───────────────────────
echo "==> Disabling wpa_supplicant.service (no WiFi)"
systemctl disable wpa_supplicant.service 2>/dev/null || true
systemctl mask    wpa_supplicant.service

# ── 4. Disable unnecessary services ──────────────────────────────────────────
echo "==> Masking unused services"
MASK=(
    cups.service
    cups-browsed.service
    ModemManager.service
    colord.service
    avahi-daemon.service
    avahi-daemon.socket
    kerneloops.service
    apport.service
    lm-sensors.service
    sysstat.service
    unattended-upgrades.service
    openvpn.service
    rtkit-daemon.service
    systemd-oomd.service
    qrtr-ns.service
    pd-mapper.service
    update-notifier-download.service
)
for svc in "${MASK[@]}"; do
    if systemctl cat "$svc" &>/dev/null 2>&1; then
        systemctl disable "$svc" 2>/dev/null || true
        systemctl mask    "$svc"
        echo "    masked $svc"
    fi
done

# ── 5. Rewrite kestrel-gnd.service ───────────────────────────────────────────
#  - Drop After=multi-user.target / Wants=multi-user.target (was gating on
#    NM-wait-online which added 5s)
#  - Drop ExecStartPre=sleep 1 (DRM is ready well before this point now)
#  - Add After=network.target so NM has configured the AR803x interface
#    before we start; the RPC client retries anyway so this is a soft dep.
echo "==> Writing optimized ${SERVICE_DST}"
cat > "${SERVICE_DST}" <<EOF
[Unit]
Description=Kestrel Ground Station OSD
After=local-fs.target network.target
Wants=network.target

[Service]
ExecStart=${BIN}
WorkingDirectory=/etc/kestrel
Restart=on-failure
RestartSec=3
User=root

StandardOutput=journal
StandardError=journal
SyslogIdentifier=kestrel-gnd

[Install]
WantedBy=multi-user.target
EOF
chmod 0644 "${SERVICE_DST}"

systemctl daemon-reload
systemctl enable kestrel-gnd.service

echo
echo "Done. Reboot to see the new boot time."
echo "  Measure after reboot:  systemd-analyze time && systemd-analyze blame | head -20"
echo "  kestrel-gnd logs:      journalctl -u kestrel-gnd -b"
