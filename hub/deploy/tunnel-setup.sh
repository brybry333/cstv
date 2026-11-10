#!/usr/bin/env bash
# Cloudflare tunnel for the hub (run on the server after server-setup.sh, as the sudo user).
# Step 1 prints a link: open it, log in to Cloudflare, pick the domain, Authorize. The tunnel then
# creates the proxied DNS records (CNAME -> <id>.cfargotunnel.com) for the domain and www itself.
set -euo pipefail
DOMAIN=${1:-cstv.xyz}
NAME=cstv
[ -f ~/.cloudflared/cert.pem ] || cloudflared tunnel login
tid() { cloudflared tunnel list -o json | python3 -c "import json,sys; print(next((t['id'] for t in json.load(sys.stdin) or [] if t['name']=='$NAME'), ''))"; }
[ -n "$(tid)" ] || cloudflared tunnel create "$NAME"
ID=$(tid)
sudo install -d -m 755 /etc/cloudflared
sudo install -m 600 ~/.cloudflared/"$ID".json /etc/cloudflared/"$ID".json
sudo tee /etc/cloudflared/config.yml >/dev/null <<CONF
tunnel: $ID
credentials-file: /etc/cloudflared/$ID.json
ingress:
  - hostname: $DOMAIN
    service: http://localhost:8787
  - hostname: www.$DOMAIN
    service: http://localhost:8787
  - service: http_status:404
CONF
cloudflared tunnel route dns "$NAME" "$DOMAIN"
cloudflared tunnel route dns "$NAME" "www.$DOMAIN"
if ! systemctl list-unit-files cloudflared.service >/dev/null 2>&1 || ! systemctl is-enabled cloudflared >/dev/null 2>&1; then
    sudo cloudflared service install
fi
sudo systemctl restart cloudflared
echo "tunnel $NAME ($ID) -> $DOMAIN, www.$DOMAIN"
