#!/usr/bin/env bash
# Streams ("Start stream" in the CS:TV window): mediamtx relays a player's RTMP push to the room's viewers.
# Installs mediamtx next to the hub, a service for it, opens TCP 1935 in the box's own firewall and tells the
# hub where streams live (CSTV_HUB_STREAM_HOST). Run from this PC:  deploy/streams-setup.sh [ssh host] [public host]
# Safe to re-run. The cloud firewall (Oracle: the VCN's security list) must also allow TCP 1935 from anywhere;
# this script can't do that part, nor for TCP 8554 (RTSP, in-game viewers) and TCP + UDP 8189 (WebRTC, the website). mediamtx asks the hub (127.0.0.1:27971) about every publish and read.
# HLS (the website's fallback for browsers whose WebRTC can't show H.264) stays on 127.0.0.1:8888: the hub passes it
# on through the site itself (/hls/<token>/...), so it needs no port.
set -euo pipefail
HOST=${1:-cstv}
PUBLIC=${2:-$(ssh -G "$HOST" | awk '$1 == "hostname" { print $2 }')}
MEDIAMTX_VERSION=v1.21.2
DIR=$(cd "$(dirname "$0")" && pwd)

scp -q "$DIR/mediamtx.yml" "$HOST:/tmp/cstv-mediamtx.yml"
ssh "$HOST" "sudo MEDIAMTX_VERSION=$MEDIAMTX_VERSION PUBLIC=$PUBLIC bash -s" <<'EOF'
set -euo pipefail
case $(uname -m) in x86_64) ARCH=amd64 ;; aarch64) ARCH=arm64 ;; *) echo "unknown CPU $(uname -m)"; exit 1 ;; esac
install -d -m 755 /opt/cstv-hub/mediamtx
sed "s/PUBLIC_HOST/$PUBLIC/" /tmp/cstv-mediamtx.yml > /opt/cstv-hub/mediamtx/mediamtx.yml
chmod 644 /opt/cstv-hub/mediamtx/mediamtx.yml
rm -f /tmp/cstv-mediamtx.yml
if ! /opt/cstv-hub/mediamtx/mediamtx --version 2>/dev/null | grep -qx "$MEDIAMTX_VERSION"; then
    tmp=$(mktemp -d)
    curl -fsSL -o "$tmp/mtx.tar.gz" \
        "https://github.com/bluenviron/mediamtx/releases/download/$MEDIAMTX_VERSION/mediamtx_${MEDIAMTX_VERSION}_linux_${ARCH}.tar.gz"
    tar -xzf "$tmp/mtx.tar.gz" -C "$tmp" mediamtx
    install -m 755 "$tmp/mediamtx" /opt/cstv-hub/mediamtx/mediamtx
    rm -rf "$tmp"
fi

cat > /etc/systemd/system/cstv-mediamtx.service <<UNIT
[Unit]
Description=CS:TV stream relay (mediamtx, RTMP 1935; the hub decides who may publish)
After=network-online.target cstv-hub.service
Wants=network-online.target

[Service]
User=cstv
Group=cstv
WorkingDirectory=/opt/cstv-hub/mediamtx
ExecStart=/opt/cstv-hub/mediamtx/mediamtx /opt/cstv-hub/mediamtx/mediamtx.yml
Restart=always
RestartSec=2
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
UNIT

# the hub: where streams are (rtmp://$PUBLIC:1935/live/<id>); its auth answers on 127.0.0.1:27971
install -d /etc/systemd/system/cstv-hub.service.d
cat > /etc/systemd/system/cstv-hub.service.d/streams.conf <<CONF
[Service]
Environment=CSTV_HUB_STREAM_HOST=$PUBLIC
CONF

# Oracle's Ubuntu image ends INPUT with a REJECT: the ACCEPTs have to go above it. 1935 = RTMP (streamers,
# in), 8554 = RTSP (in-game viewers), 8189 = WebRTC media for the website (UDP, and TCP when UDP is blocked).
for spec in "tcp 1935" "tcp 8554" "tcp 8189" "udp 8189"; do
    set -- $spec
    if [ "$1" = tcp ]; then rule=(-p tcp -m state --state NEW -m tcp --dport "$2" -j ACCEPT)
    else rule=(-p udp -m udp --dport "$2" -j ACCEPT); fi
    if ! iptables -C INPUT "${rule[@]}" 2>/dev/null; then
        pos=$(iptables -L INPUT --line-numbers -n | awk '$2 == "REJECT" { print $1; exit }')
        if [ -n "$pos" ]; then iptables -I INPUT "$pos" "${rule[@]}"; else iptables -A INPUT "${rule[@]}"; fi
    fi
done
if command -v netfilter-persistent >/dev/null; then netfilter-persistent save >/dev/null 2>&1 || true
else DEBIAN_FRONTEND=noninteractive apt-get install -y -q iptables-persistent >/dev/null && netfilter-persistent save >/dev/null 2>&1 || true; fi

systemctl daemon-reload
systemctl enable cstv-mediamtx >/dev/null
systemctl restart cstv-hub cstv-mediamtx
sleep 1.5
systemctl is-active cstv-hub cstv-mediamtx
EOF
echo "streams: rtmp://$PUBLIC:1935 (remember the cloud firewall: TCP 1935 from 0.0.0.0/0)"
