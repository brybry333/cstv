#!/usr/bin/env bash
# Copy the hub (hub.py, web/, PROTOCOL.md) to the server and restart it. Data (hub.db, giphy.key) stays there.
# usage: deploy.sh [ssh host alias, default cstv]   (~/.ssh/config: Host cstv -> the server, user ubuntu)
set -euo pipefail
HOST=${1:-cstv}
HUB=$(dirname "$(readlink -f "$0")")/..
rsync -az --delete --exclude data/ --exclude deploy/ --exclude browsertest/ --exclude __pycache__/ "$HUB"/ "$HOST":/opt/cstv-hub/app/
ssh "$HOST" 'sudo systemctl restart cstv-hub && sleep 1.5 && systemctl is-active cstv-hub && curl -fsS localhost:8787/healthz && echo'
