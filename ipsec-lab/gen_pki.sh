#!/usr/bin/env bash
# =============================================================================
# gen_pki.sh — Regenerate all PKI certificates for the IPSec lab
#
# Run this if you want fresh certificates (the ones in pki/ are demo certs
# that work out of the box — you only need this script if you want to change
# the CN/SAN values or rotate keys).
#
# REQUIRES: openssl (installed by default on Linux/Mac)
#
# USAGE:
#   chmod +x gen_pki.sh
#   ./gen_pki.sh
#
# After running, rebuild the containers:
#   docker-compose down
#   docker-compose up --build
# =============================================================================

set -euo pipefail

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info() { echo -e "${GREEN}[PKI]${NC}  $*"; }
warn() { echo -e "${YELLOW}[PKI]${NC}  $*"; }

DAYS=3650   # 10-year validity (demo only — use 1 year for production)
PKI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/pki"

# Clean and recreate
rm -rf "$PKI_DIR"
mkdir -p "$PKI_DIR/ca" "$PKI_DIR/siteA" "$PKI_DIR/siteB"

# ─── 1. CA (Certificate Authority) ──────────────────────────────────────────
info "Generating CA key and self-signed certificate..."

openssl genrsa -out "$PKI_DIR/ca/ca.key" 4096 2>/dev/null
openssl req -x509 -new -nodes \
    -key "$PKI_DIR/ca/ca.key" \
    -sha256 -days $DAYS \
    -subj "/C=IN/O=IPSec-Lab-Demo/CN=Demo-CA" \
    -out "$PKI_DIR/ca/ca.crt"

info "CA certificate: $PKI_DIR/ca/ca.crt"

# ─── 2. Site A certificate ───────────────────────────────────────────────────
info "Generating Site A key and certificate..."

openssl genrsa -out "$PKI_DIR/siteA/siteA.key" 2048 2>/dev/null

openssl req -new \
    -key "$PKI_DIR/siteA/siteA.key" \
    -subj "/C=IN/O=IPSec-Lab-Demo/CN=site-a" \
    -out "$PKI_DIR/siteA/siteA.csr"

# SAN must match the container's IP / hostname used in swanctl.conf
openssl x509 -req \
    -in "$PKI_DIR/siteA/siteA.csr" \
    -CA "$PKI_DIR/ca/ca.crt" \
    -CAkey "$PKI_DIR/ca/ca.key" \
    -CAcreateserial \
    -days $DAYS -sha256 \
    -extfile <(printf "subjectAltName=IP:172.20.0.10,DNS:site-a") \
    -out "$PKI_DIR/siteA/siteA.crt" 2>/dev/null

info "Site A certificate: $PKI_DIR/siteA/siteA.crt  (SAN: 172.20.0.10 / site-a)"

# ─── 3. Site B certificate ───────────────────────────────────────────────────
info "Generating Site B key and certificate..."

openssl genrsa -out "$PKI_DIR/siteB/siteB.key" 2048 2>/dev/null

openssl req -new \
    -key "$PKI_DIR/siteB/siteB.key" \
    -subj "/C=IN/O=IPSec-Lab-Demo/CN=site-b" \
    -out "$PKI_DIR/siteB/siteB.csr"

openssl x509 -req \
    -in "$PKI_DIR/siteB/siteB.csr" \
    -CA "$PKI_DIR/ca/ca.crt" \
    -CAkey "$PKI_DIR/ca/ca.key" \
    -CAcreateserial \
    -days $DAYS -sha256 \
    -extfile <(printf "subjectAltName=IP:172.20.0.20,DNS:site-b") \
    -out "$PKI_DIR/siteB/siteB.crt" 2>/dev/null

info "Site B certificate: $PKI_DIR/siteB/siteB.crt  (SAN: 172.20.0.20 / site-b)"

# ─── 4. Verify ───────────────────────────────────────────────────────────────
info "Verifying Site A cert against CA..."
openssl verify -CAfile "$PKI_DIR/ca/ca.crt" "$PKI_DIR/siteA/siteA.crt"

info "Verifying Site B cert against CA..."
openssl verify -CAfile "$PKI_DIR/ca/ca.crt" "$PKI_DIR/siteB/siteB.crt"

warn "These are self-signed DEMO certificates — do NOT use in production."
info "PKI generation complete. Run: docker-compose up --build"
