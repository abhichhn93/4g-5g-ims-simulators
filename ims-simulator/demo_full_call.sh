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
UE_C_PIPE=$(mktemp -u /tmp/ims_ue_c_XXXX)
mkfifo "$SERVER_PIPE" "$UE_A_PIPE" "$UE_B_PIPE" "$UE_C_PIPE"

cleanup() {
    pkill -f ims_server 2>/dev/null || true
    pkill -f ue_sim     2>/dev/null || true
    rm -f "$SERVER_PIPE" "$UE_A_PIPE" "$UE_B_PIPE" "$UE_C_PIPE"
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

# ── Start UE-C ────────────────────────────────────────────────
banner "Step 3b — Starting UE-C (conference participant)"
./ue_sim C < "$UE_C_PIPE" > ims_C_demo.log 2>&1 &
UE_C_PID=$!
exec 6>"$UE_C_PIPE"
wait_s 1

# ── IMS Registration ─────────────────────────────────────────
banner "Step 4 — SIP REGISTER (all 3 UEs)"
step "UE-A: REG"
echo "REG" >&4
wait_s $DELAY

step "UE-B: REG"
echo "REG" >&5
wait_s $DELAY

step "UE-C: REG"
echo "REG" >&6
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

# ── CONFERENCE ───────────────────────────────────────────────
banner "Step 8 — CONFERENCE (REFER → INVITE UE-C → 3-way call)"
step "UE-A: CONF C  →  REFER → 202 Accepted → NOTIFY (trying)"
echo "CONF C" >&4
wait_s $DELAY

step "UE-C: ACCEPT  →  180 Ringing → 200 OK → ACK → NOTIFY (terminated)"
echo "ACCEPT" >&6
wait_s $DELAY

# ── HOLD ─────────────────────────────────────────────────────
banner "Step 9 — HOLD  (re-INVITE a=sendonly)"
step "UE-A: HOLD"
echo "HOLD" >&4
wait_s $DELAY

# ── RESUME ───────────────────────────────────────────────────
banner "Step 10 — RESUME  (re-INVITE a=sendrecv)"
step "UE-A: RESUME"
echo "RESUME" >&4
wait_s $DELAY

# ── BYE ──────────────────────────────────────────────────────
banner "Step 11 — BYE  (call teardown)"
step "UE-A: BYE  →  200 OK BYE  →  Rx STR → PCRF → QCI=1 released"
echo "BYE" >&4
wait_s $DELAY

# ── Shutdown ─────────────────────────────────────────────────
banner "Step 12 — Shutdown"
echo "QUIT" >&4
echo "QUIT" >&5
echo "QUIT" >&6
wait_s 1
echo "QUIT" >&3
wait_s 2

# ── Merge pcaps ──────────────────────────────────────────────
banner "Step 13 — Merging all 4 pcap files"
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
ls -lh ims_server_capture.pcap ims_A_capture.pcap ims_B_capture.pcap ims_C_capture.pcap ims_combined.pcap 2>/dev/null || true
echo ""
echo -e "${GREEN}  Terminal logs:${RESET}"
ls -lh "$SERVER_LOG" "$UE_A_LOG" "$UE_B_LOG" ims_C_demo.log 2>/dev/null || true
echo ""
echo -e "${YELLOW}══════════════ WIRESHARK FILTERS ════════════════${RESET}"
echo ""
echo -e "${CYAN}  By message type:${RESET}"
echo "    sip                                  → all SIP"
echo "    sip.Method == \"REGISTER\"             → registration"
echo "    sip.Method == \"INVITE\"               → calls + conference leg"
echo "    sip.Method == \"PRACK\"                → reliable provisional ACK"
echo "    sip.Method == \"REFER\"                → conference trigger"
echo "    sip.Method == \"NOTIFY\"               → REFER status + conf-state XML"
echo "    sip.Method == \"SUBSCRIBE\"            → conference-state subscription"
echo "    sip.Method == \"BYE\"                  → teardown"
echo "    sip.Status-Code == 100               → 100 Trying"
echo "    sip.Status-Code == 180               → 180 Ringing"
echo "    sip.Status-Code == 183               → 183 Session Progress (QoS)"
echo "    sip.Status-Code == 200               → all 200 OK"
echo "    sip.Status-Code == 202               → 202 Accepted (REFER response)"
echo "    diameter                             → Cx SAR/SAA + Rx AAR"
echo ""
echo -e "${CYAN}  By call leg (Call-ID):${RESET}"
echo "    sip.Call-ID contains \":B-2\"          → A↔B main call leg"
echo "    sip.Call-ID contains \"-conf\"         → conference INVITE to UE-C"
echo "    sip.Call-ID contains \"-sub\"          → SUBSCRIBE/NOTIFY conference-state"
echo ""
echo -e "${CYAN}  By participant (IP address):${RESET}"
echo "    ip.addr == 10.0.0.1                  → UE-A"
echo "    ip.addr == 10.0.0.2                  → UE-B"
echo "    ip.addr == 10.0.0.3                  → UE-C"
echo "    ip.addr == 10.0.0.8                  → P-CSCF"
echo "    ip.addr == 10.0.0.9                  → S-CSCF"
echo "    ip.addr == 10.0.0.11                 → MRFC (conference bridge)"
echo ""
echo -e "${CYAN}  Conference only:${RESET}"
echo "    sip.Method == \"REFER\" or sip.Method == \"NOTIFY\""
echo "    or sip.Status-Code == 202"
echo ""
echo -e "${GREEN}  Full packet sequence:${RESET}"
echo ""
echo "  REGISTRATION"
echo "    REGISTER (A/B/C) → 200 OK REGISTER"
echo ""
echo "  CALL SETUP (A→B)"
echo "    INVITE → 100 Trying → 183 Session Progress+SDP"
echo "    → PRACK → 200 OK PRACK"
echo "    → 180 Ringing → 200 OK INVITE+SDP → ACK"
echo ""
echo "  CONFERENCE (A adds C)"
echo "    REFER (Refer-To:UE-C) → 202 Accepted"
echo "    NOTIFY trying  (body: SIP/2.0 100 Trying)"
echo "    SUBSCRIBE (conf-state) → 200 OK"
echo "    INVITE (Call-ID=-conf) → 100 Trying → 183 → PRACK → 200 OK PRACK"
echo "    → NOTIFY early (body: SIP/2.0 180 Ringing)"
echo "    → 180 Ringing → 200 OK INVITE → ACK"
echo "    NOTIFY terminated (body: SIP/2.0 200 OK)"
echo "    NOTIFY (conference-info+xml: UE-A, UE-B, UE-C all connected)"
echo ""
echo "  MID-CALL"
echo "    re-INVITE (a=sendonly HOLD) → 200 OK"
echo "    re-INVITE (a=sendrecv RESUME) → 200 OK"
echo ""
echo "  TEARDOWN"
echo "    BYE → 200 OK BYE"
echo ""

# ── Show server log summary (last 60 lines) ───────────────────
banner "Server log tail (last 60 lines)"
tail -60 "$SERVER_LOG"
