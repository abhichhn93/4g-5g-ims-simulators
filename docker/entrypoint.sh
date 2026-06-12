#!/bin/sh
# Entrypoint for the mme-sim container.
#
# mme_sim is an interactive CLI tool (reads commands from stdin until QUIT).
# "CR 1" runs one full LTE Attach flow for 1 UE and writes mme_capture.pcap.
# After it finishes, we move the pcap into /data and serve that folder over
# HTTP so it can be downloaded through a Kubernetes Service.
set -e

mkdir -p /data

echo "=== Running MME simulator (1 UE attach flow) ==="
printf 'CR 1\nQUIT\n' | ./mme_sim

mv -f mme_capture.pcap /data/ 2>/dev/null || true

echo "=== Done. PCAP available at /data/mme_capture.pcap ==="
echo "=== Serving /data on :8080 ==="
cd /data && exec python3 -m http.server 8080
