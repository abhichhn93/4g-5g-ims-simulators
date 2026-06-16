#!/usr/bin/env bash
# run_demo.sh — One-command demo: starts all simulator nodes + visualizer,
# opens the browser, then runs a preset call-flow scenario.
# Usage: bash tools/run_demo.sh [4g|5g|ims]    (default: 4g)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-4g}"

# ── Colour helpers ──────────────────────────────────────────────────────────
GREEN='\033[1;32m'; BLUE='\033[1;34m'; RESET='\033[0m'
info() { echo -e "${GREEN}[run_demo]${RESET} $*"; }
step() { echo -e "${BLUE}  ──${RESET} $*"; }

wait_port() {
    local host="$1" port="$2" label="$3" deadline=$((SECONDS+15))
    until nc -z "$host" "$port" 2>/dev/null; do
        [[ $SECONDS -gt $deadline ]] && { echo "[run_demo] TIMEOUT waiting for $label"; exit 1; }
        sleep 0.5
    done
    info "$label ready"
}

cleanup() {
    info "Shutting down all background processes…"
    jobs -p | xargs -r kill 2>/dev/null || true
}
trap cleanup EXIT

info "=== Telecom Simulator Demo — mode: ${MODE} ==="

# ── Check dependencies ──────────────────────────────────────────────────────
if ! python3 -c "import websockets, yaml" 2>/dev/null; then
    info "Installing Python deps: pip install websockets pyyaml"
    pip install --quiet websockets pyyaml
fi

# ── Start visualizer first (so it catches events from the start) ─────────────
step "Starting viz_server.py on ws://localhost:8765 + http://localhost:8080"
python3 "$REPO/tools/viz_server.py" &
VIZ_PID=$!
sleep 1

# ── Start simulator nodes ────────────────────────────────────────────────────
case "$MODE" in
  4g)
    SIMDIR="$REPO/4g-simulator"
    [[ ! -f "$SIMDIR/build/mme_sim" ]] && { step "Building 4G…"; (cd "$SIMDIR" && cmake -B build -DCMAKE_BUILD_TYPE=Release -q && cmake --build build -q); }
    step "Starting 4G EPC (MME+HSS+SGW+PGW+PCRF+eNB)…"
    (cd "$SIMDIR" && ./build/mme_sim) &
    sleep 2
    info "Running preset scenario: ATTACH 1  TAU 1  HO 1  BULK 3  QUIT"
    sleep 1
    (cd "$SIMDIR" && printf 'CR 1\nTAU 1\nHO 1\nBULK 3\nQUIT\n' | ./build/mme_sim > /dev/null 2>&1 &)
    ;;

  5g)
    SIMDIR="$REPO/5g-simulator"
    [[ ! -f "$SIMDIR/build/amf_sim" ]] && { step "Building 5G…"; (cd "$SIMDIR" && cmake -B build -DCMAKE_BUILD_TYPE=Release -q && cmake --build build -q); }
    step "Starting 5G NRF…"
    (cd "$SIMDIR" && ./build/nrf_sim) &
    sleep 1
    step "Starting UDM…"; (cd "$SIMDIR" && ./build/udm_sim) &
    sleep 0.5
    step "Starting SMF…"; (cd "$SIMDIR" && ./build/smf_sim) &
    sleep 0.5
    step "Starting UPF…"; (cd "$SIMDIR" && ./build/upf_sim) &
    sleep 0.5
    step "Starting AMF…"; (cd "$SIMDIR" && ./build/amf_sim) &
    sleep 1
    info "Running preset scenario: REG 1  PDU 1  REG 2  PDU 2  QUIT"
    (cd "$SIMDIR" && printf 'REG 1\nPDU 1 1 internet\nREG 2\nPDU 2 1 internet\nQUIT\n' | ./build/gnb_sim > /dev/null 2>&1 &)
    ;;

  ims)
    SIMDIR="$REPO/ims-simulator"
    [[ ! -f "$SIMDIR/build/ims_server" ]] && { step "Building IMS…"; (cd "$SIMDIR" && cmake -B build -DCMAKE_BUILD_TYPE=Release -q && cmake --build build -q); }
    step "Starting IMS server…"
    (cd "$SIMDIR" && ./build/ims_server) &
    sleep 2
    info "Running preset scenario: REG ALL  CALL A B  BYE  QUIT"
    (cd "$SIMDIR" && printf 'REG ALL\nCALL A B\nBYE\nQUIT\n' | ./build/ue_sim > /dev/null 2>&1 &)
    ;;

  *)
    echo "Usage: $0 [4g|5g|ims]"
    exit 1
    ;;
esac

# ── Open browser ─────────────────────────────────────────────────────────────
info "Opening visualizer at http://localhost:8080"
if command -v open &>/dev/null; then
    open "http://localhost:8080"        # macOS
elif command -v xdg-open &>/dev/null; then
    xdg-open "http://localhost:8080"    # Linux
else
    info "Please open http://localhost:8080 in your browser manually"
fi

echo ""
info "Visualizer running at http://localhost:8080"
info "Press Ctrl+C to stop all processes"
wait $VIZ_PID
