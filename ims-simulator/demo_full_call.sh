#!/bin/bash
# ============================================================
# IMS Full Call Demo Script
# Runs a complete REG → CALL → HOLD → RESUME → BYE flow
# Captures:
#   - ims_server_capture.pcap  (server side — all SIP + Diameter)
#   - ims_A_capture.pcap       (UE-A view)
#   - ims_B_capture.pcap       (UE-B view)
#   - ims_combined.pcap        (merged, open this in Wireshark)
#
# Usage:  ./demo_full_call.sh [BEGINNER|ENGINEER]
# ============================================================

set -e
cd "$(dirname "$0")/build"

LOG_LEVEL="${1:-ENGINEER}"
DELAY=3     # seconds between commands (increase if you want to read logs)

CYAN='\033[1;36m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
RESET='\033[0m'

banner() { echo -e "\n${CYAN}══════════════════════════════════════════${RESET}"; \
           echo -e "${CYAN}  $1${RESET}"; \
           echo -e "${CYAN}══════════════════════════════════════════${RESET}"; }

step()  { echo -e "${GREEN}▶ $1${RESET}"; sleep 0.5; }
wait_s(){ echo -e "${YELLOW}  ⏳ waiting ${1}s...${RESET}"; sleep "$1"; }

banner "IMS Demo — LOG_LEVEL=$LOG_LEVEL"

# ── Cleanup stale processes ──────────────────────────────────
pkill -f ims_server 2>/dev/null || true
pkill -f ue_sim     2>/dev/null || true
sleep 1

# ── Output files ─────────────────────────────────────────────
SERVER_LOG="ims_server_demo.log"
UE_A_LOG="ims_A_demo.log"
UE_B_LOG="ims_B_demo.log"
rm -f "$SERVER_LOG" "$UE_A_LOG" "$UE_B_LOG"
rm -f ims_server_capture.pcap ims_A_capture.pcap ims_B_capture.pcap ims_combined.pcap

# ── Pipe FIFOs ────────────────────────────────────────────────
SERVER_PIPE=$(mktemp -u /tmp/ims_server_XXXX)
UE_A_PIPE=$(mktemp -u /tmp/ims_ue_a_XXXX)
UE_B_PIPE=$(mktemp -u /tmp/ims_ue_b_XXXX)
mkfifo "$SERVER_PIPE" "$UE_A_PIPE" "$UE_B_PIPE"

cleanup() {
    pkill -f ims_server 2>/dev/null || true
    pkill -f ue_sim     2>/dev/null || true
    rm -f "$SERVER_PIPE" "$UE_A_PIPE" "$UE_B_PIPE"
}
trap cleanup EXIT

# ── Start server ──────────────────────────────────────────────
banner "Step 1 — Starting IMS server (P-CSCF + S-CSCF + IMS-HSS)"
LOG_LEVEL="$LOG_LEVEL" ./ims_server < "$SERVER_PIPE" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
exec 3>"$SERVER_PIPE"   # keep pipe open so server doesn't see EOF
wait_s 2

# ── Start UE-A ────────────────────────────────────────────────
banner "Step 2 — Starting UE-A (caller)"
./ue_sim A < "$UE_A_PIPE" > "$UE_A_LOG" 2>&1 &
UE_A_PID=$!
exec 4>"$UE_A_PIPE"
wait_s 1

# ── Start UE-B ────────────────────────────────────────────────
banner "Step 3 — Starting UE-B (callee)"
./ue_sim B < "$UE_B_PIPE" > "$UE_B_LOG" 2>&1 &
UE_B_PID=$!
exec 5>"$UE_B_PIPE"
wait_s 1

# ── IMS Registration ─────────────────────────────────────────
banner "Step 4 — SIP REGISTER (UE-A)"
step "UE-A: REG"
echo "REG" >&4
wait_s $DELAY

banner "Step 5 — SIP REGISTER (UE-B)"
step "UE-B: REG"
echo "REG" >&5
wait_s $DELAY

# Check STATUS
step "Server: STATUS (shows registered UEs)"
echo "STATUS" >&3
wait_s 1

# ── VoLTE Call Setup ─────────────────────────────────────────
banner "Step 6 — SIP INVITE  (UE-A → UE-B VoLTE call)"
step "UE-A: CALL B  →  100 Trying → 180 Ringing → PRACK → 200 OK → ACK"
echo "CALL B" >&4
wait_s $DELAY

banner "Step 7 — UE-B answers the call"
step "UE-B: ACCEPT  →  200 OK with SDP answer (AMR-WB/16000)"
echo "ACCEPT" >&5
wait_s $DELAY

# ── HOLD ─────────────────────────────────────────────────────
banner "Step 8 — HOLD  (re-INVITE a=sendonly)"
step "UE-A: HOLD"
echo "HOLD" >&4
wait_s $DELAY

# ── RESUME ───────────────────────────────────────────────────
banner "Step 9 — RESUME  (re-INVITE a=sendrecv)"
step "UE-A: RESUME"
echo "RESUME" >&4
wait_s $DELAY

# ── BYE ──────────────────────────────────────────────────────
banner "Step 10 — BYE  (call teardown)"
step "UE-A: BYE  →  200 OK BYE  →  Rx STR → PCRF → QCI=1 released"
echo "BYE" >&4
wait_s $DELAY

# ── Shutdown ─────────────────────────────────────────────────
banner "Step 11 — Shutdown"
echo "QUIT" >&4
echo "QUIT" >&5
wait_s 1
echo "QUIT" >&3
wait_s 2

# ── Merge pcaps ──────────────────────────────────────────────
banner "Step 12 — Merging all 3 pcap files"
if [ -f ./merge_pcap.sh ]; then
    bash ./merge_pcap.sh
else
    # fallback: use mergecap if available
    if command -v mergecap &>/dev/null; then
        mergecap -w ims_combined.pcap \
            ims_server_capture.pcap \
            ims_A_capture.pcap \
            ims_B_capture.pcap 2>/dev/null || true
    fi
fi

# ── Print summary ─────────────────────────────────────────────
banner "DONE — Files generated"
echo ""
echo -e "${GREEN}  PCAP files (open in Wireshark):${RESET}"
ls -lh ims_server_capture.pcap ims_A_capture.pcap ims_B_capture.pcap ims_combined.pcap 2>/dev/null || true
echo ""
echo -e "${GREEN}  Terminal logs (cat to read):${RESET}"
ls -lh "$SERVER_LOG" "$UE_A_LOG" "$UE_B_LOG"
echo ""
echo -e "${YELLOW}  Wireshark filters to use:${RESET}"
echo "    sip                        → all SIP messages"
echo "    sip.Method == \"REGISTER\"   → registration only"
echo "    sip.Method == \"INVITE\"     → INVITE + re-INVITE"
echo "    sip.Method == \"PRACK\"      → reliable provisional ACK"
echo "    sip.Status-Code == 100     → 100 Trying"
echo "    sip.Status-Code == 180     → 180 Ringing"
echo "    sip.Status-Code == 200     → 200 OK (all methods)"
echo "    sip.Method == \"BYE\"        → call teardown"
echo "    diameter                   → Cx SAR/SAA + Rx AAR"
echo ""
echo -e "${GREEN}  Expected packet sequence in ims_combined.pcap:${RESET}"
echo "    REGISTER → 200 OK (REG)"
echo "    INVITE → 100 Trying → 180 Ringing → PRACK → 200 OK (PRACK)"
echo "    → 200 OK (INVITE) → ACK"
echo "    re-INVITE (HOLD a=sendonly) → 200 OK"
echo "    re-INVITE (RESUME a=sendrecv) → 200 OK"
echo "    BYE → 200 OK (BYE)"
echo "    Diameter: SAR/SAA + Rx AAR"
echo ""

# ── Show server log summary (last 60 lines) ───────────────────
banner "Server log tail (last 60 lines)"
tail -60 "$SERVER_LOG"
