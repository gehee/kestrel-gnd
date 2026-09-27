#!/usr/bin/env bash
#
# install-service.sh — install kestrel-gnd as a systemd service on a Linux
# ground station (RK3588 goggles / x86_64 workstation).
#
# It:
#   1. Creates /etc/kestrel and installs the idle-screen assets. The app loads
#      hardcoded /etc/kestrel/background.{png,mp4}, so the "village" assets are
#      copied under those names:
#        village.png             -> /etc/kestrel/background.png
#        village_1080p_60fps.mp4 -> /etc/kestrel/background.mp4
#   2. Generates, enables and (re)starts the kestrel-gnd systemd unit, pointing
#      at the binary built in ./build.
#
# Usage:  sudo ./install-service.sh
#         (re-runnable / idempotent — safe to run again to update assets)

set -euo pipefail

# Directory this script lives in (kestrel-gnd), regardless of CWD.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- must be root: writes to /etc and manages systemd -----------------------
if [[ ${EUID} -ne 0 ]]; then
    echo "==> Needs root; re-running with sudo..." >&2
    exec sudo -- "$0" "$@"
fi

ASSET_DIR="/etc/kestrel"
SERVICE_NAME="kestrel-gnd.service"
SERVICE_DST="/etc/systemd/system/${SERVICE_NAME}"
BIN="${SCRIPT_DIR}/build/kestrel-gnd"

BG_PNG_SRC="${SCRIPT_DIR}/village.png"
BG_MP4_SRC="${SCRIPT_DIR}/village_1080p_60fps.mp4"
YAML_SRC="${SCRIPT_DIR}/kestrel-gnd.yaml"

# --- 1. the binary must already be built ------------------------------------
if [[ ! -x "${BIN}" ]]; then
    echo "ERROR: kestrel-gnd binary not found at:" >&2
    echo "         ${BIN}" >&2
    echo "       Build it first (as your normal user, not root):" >&2
    echo "         cd \"${SCRIPT_DIR}\"" >&2
    echo "         cmake -S . -B build -DUSE_RKMPP=ON && cmake --build build -j" >&2
    exit 1
fi

# --- 2. install idle-screen assets ------------------------------------------
for f in "${BG_PNG_SRC}" "${BG_MP4_SRC}"; do
    if [[ ! -f "${f}" ]]; then
        echo "ERROR: missing asset: ${f}" >&2
        exit 1
    fi
done

echo "==> Installing assets into ${ASSET_DIR}"
install -d -m 0755 "${ASSET_DIR}"
install -m 0644 "${BG_PNG_SRC}" "${ASSET_DIR}/background.png"
install -m 0644 "${BG_MP4_SRC}" "${ASSET_DIR}/background.mp4"
echo "    village.png             -> ${ASSET_DIR}/background.png"
echo "    village_1080p_60fps.mp4 -> ${ASSET_DIR}/background.mp4"

# Settings yaml — only install if not already present so local edits survive re-runs.
if [[ ! -f "${ASSET_DIR}/kestrel-gnd.yaml" ]]; then
    if [[ -f "${YAML_SRC}" ]]; then
        install -m 0644 "${YAML_SRC}" "${ASSET_DIR}/kestrel-gnd.yaml"
        echo "    kestrel-gnd.yaml        -> ${ASSET_DIR}/kestrel-gnd.yaml"
    fi
else
    echo "    kestrel-gnd.yaml already exists in ${ASSET_DIR}, skipping (edit in place to change settings)"
fi

# --- 3. generate the systemd unit -------------------------------------------
echo "==> Writing ${SERVICE_DST}"
cat > "${SERVICE_DST}" <<EOF
[Unit]
Description=Kestrel Ground Station OSD
After=local-fs.target network.target
Wants=network.target

[Service]
ExecStart=${BIN}
WorkingDirectory=${ASSET_DIR}
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

# --- 4. enable + (re)start --------------------------------------------------
echo "==> Enabling and starting ${SERVICE_NAME}"
systemctl daemon-reload
systemctl enable "${SERVICE_NAME}"
systemctl restart "${SERVICE_NAME}"

echo
echo "Done — kestrel-gnd installed and running."
echo "  Status:  systemctl status ${SERVICE_NAME}"
echo "  Logs:    journalctl -u ${SERVICE_NAME} -f"
echo "  Stop:    systemctl stop ${SERVICE_NAME}"
