#!/bin/bash
# Startup script for siteA container

# Add a fake "LAN" interface so we have traffic to send through the tunnel
# This simulates siteA having an internal network 10.0.0.0/24
ip addr add 10.0.0.1/24 dev lo 2>/dev/null || true

# Start the strongSwan IKE daemon via the starter wrapper
# 'ipsec start' launches charon + sets up logging properly on Alpine
echo "[siteA] Starting strongSwan charon daemon..."
/usr/lib/strongswan/charon &
CHARON_PID=$!

# Wait for charon to initialize
sleep 2

# Load our swanctl config (certs + connection definitions)
echo "[siteA] Loading swanctl config..."
swanctl --load-all

echo "[siteA] IPSec tunnel initiating to siteB..."
echo "[siteA] Container ready. Keeping alive..."

# Keep container running and show logs
tail -f /var/log/charon.log 2>/dev/null || wait $CHARON_PID
