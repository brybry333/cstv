#!/usr/bin/env bash
# One-time setup of the CS:TV hub server (Ubuntu 24.04, run as the default sudo user, e.g. `ubuntu`).
# Installs: security updates (auto, reboot 04:30 when a kernel needs it), python venv + websockets, the
# cstv-hub service (runs as user `cstv`, data in /var/lib/cstv-hub), cloudflared (Cloudflare's apt repo).
# The Cloudflare tunnel itself is set up afterwards (tunnel-setup.sh: needs a browser login).
set -euo pipefail
PUBLIC_URL=${1:-https://cstv.xyz}
export DEBIAN_FRONTEND=noninteractive

# small servers (Oracle's free E2.1.Micro has 1 GB): a 2 GB swap file so upgrades can't run out of memory
if [ "$(awk '/MemTotal/ {print $2}' /proc/meminfo)" -lt 2000000 ] && ! swapon --show | grep -q .; then
    sudo fallocate -l 2G /swapfile && sudo chmod 600 /swapfile && sudo mkswap -q /swapfile && sudo swapon /swapfile
    grep -q '^/swapfile' /etc/fstab || echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab >/dev/null
fi

sudo apt-get update -q
sudo apt-get -y -q upgrade
sudo apt-get install -y -q python3-venv rsync unattended-upgrades curl

# automatic security updates, reboot at 04:30 only when an update needs it
sudo tee /etc/apt/apt.conf.d/52cstv-unattended >/dev/null <<'CONF'
Unattended-Upgrade::Automatic-Reboot "true";
Unattended-Upgrade::Automatic-Reboot-Time "04:30";
CONF
sudo tee /etc/apt/apt.conf.d/20auto-upgrades >/dev/null <<'CONF'
APT::Periodic::Update-Package-Lists "1";
APT::Periodic::Unattended-Upgrade "1";
CONF

# service user, code (owned by the deploy user), data (owned by the service)
id cstv >/dev/null 2>&1 || sudo useradd --system --home-dir /var/lib/cstv-hub --shell /usr/sbin/nologin cstv
sudo install -d -o "$USER" -g "$USER" /opt/cstv-hub /opt/cstv-hub/app
sudo install -d -o cstv -g cstv -m 750 /var/lib/cstv-hub
[ -x /opt/cstv-hub/venv/bin/python ] || python3 -m venv /opt/cstv-hub/venv
/opt/cstv-hub/venv/bin/pip install -q --upgrade pip 'websockets>=13'

sudo tee /etc/systemd/system/cstv-hub.service >/dev/null <<UNIT
[Unit]
Description=CS:TV hub (lobbies, website) for $PUBLIC_URL
After=network-online.target
Wants=network-online.target

[Service]
User=cstv
Group=cstv
WorkingDirectory=/opt/cstv-hub/app
Environment=CSTV_HUB_PUBLIC_URL=$PUBLIC_URL
Environment=CSTV_HUB_TRUST_CF=1
Environment=CSTV_HUB_DB=/var/lib/cstv-hub/hub.db
ExecStart=/opt/cstv-hub/venv/bin/python /opt/cstv-hub/app/hub.py
Restart=always
RestartSec=2
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
ReadWritePaths=/var/lib/cstv-hub

[Install]
WantedBy=multi-user.target
UNIT
sudo systemctl daemon-reload
sudo systemctl enable cstv-hub >/dev/null

# cloudflared from Cloudflare's own apt repository
if ! command -v cloudflared >/dev/null; then
    sudo install -d -m 0755 /usr/share/keyrings
    curl -fsSL https://pkg.cloudflare.com/cloudflare-main.gpg | sudo tee /usr/share/keyrings/cloudflare-main.gpg >/dev/null
    echo 'deb [signed-by=/usr/share/keyrings/cloudflare-main.gpg] https://pkg.cloudflare.com/cloudflared any main' \
        | sudo tee /etc/apt/sources.list.d/cloudflared.list >/dev/null
    sudo apt-get update -q
    sudo apt-get install -y -q cloudflared
fi
echo "server setup done: $(cloudflared --version | head -1); python $(/opt/cstv-hub/venv/bin/python -V)"
