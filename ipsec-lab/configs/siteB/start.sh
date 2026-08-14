#!/bin/bash
# Startup script for siteB container

ip addr add 10.0.1.1/24 dev lo 2>/dev/null || true

echo "[siteB] Starting strongSwan charon daemon..."
/usr/lib/strongswan/charon &
CHARON_PID=$!

sleep 2

echo "[siteB] Loading swanctl config..."
swanctl --load-all

echo "[siteB] Waiting for siteA to initiate tunnel..."
echo "[siteB] Container ready."

tail -f /var/log/charon.log 2>/dev/null || wait $CHARON_PID
