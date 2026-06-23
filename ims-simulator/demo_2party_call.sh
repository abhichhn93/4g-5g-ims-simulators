#!/bin/bash
# ============================================================
# IMS 2-Party Call Demo
# REG A + REG B → CALL → ACCEPT → HOLD → RESUME → BYE
#
# Generates:
#   build/ims_server_demo.log   — full server log (P-CSCF/S-CSCF/MTAS)
#   build/ims_A_demo.log        — UE-A terminal log
#   build/ims_B_demo.log        — UE-B terminal log
#   build/ims_combined.pcap     — merged pcap (open in Wireshark)
#
# Usage:
#   ./demo_2party_call.sh              # ENGINEER mode (raw IEs)
#   ./demo_2party_call.sh BEGINNER     # plain-English mode
# ============================================================

set -e
cd "$(dirname "$0")/build"

LOG_LEVEL="${1:-ENGINEER}"
DELAY=3

CYAN='\033[1;36m'; GREEN='\033[1;32m'; YELLOW='\033[1;33m'; RESET='\033[0m'

banner() { echo -e "\n${CYAN}══════════════════════════════════════${RESET}"; \
           echo -e "${CYAN}  $1${RESET}"; \
           echo -e "${CYAN}══════════════════════════════════════${RESET}"; }
step()   { echo -e "${GREEN}▶ $1${RESET}"; sleep 0.3; }
wait_s() { echo -e "${YELLOW}  ⏳ ${1}s...${RESET}"; sleep "$1"; }

banner "2-Party Call Demo — LOG_LEVEL=$LOG_LEVEL"

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

# ── Named pipes for stdin ─────────────────────────────────────
SERVER_PIPE=$(mktemp -u /tmp/ims_server_XXXX)
UE_A_PIPE=$(mktemp -u /tmp/ims_ue_a_XXXX)
UE_B_PIPE=$(mktemp -u /tmp/ims_ue_b_XXXX)
mkfifo "$SERVER_PIPE" "$UE_A_PIPE" "$UE_B_PIPE"

cleanup() {
    kill "$SERVER_PID" "$UE_A_PID" "$UE_B_PID" 2>/dev/null || true
    rm -f "$SERVER_PIPE" "$UE_A_PIPE" "$UE_B_PIPE"
}
trap cleanup EXIT

# ── Start IMS server ─────────────────────────────────────────
banner "Step 1 — IMS server  (P-CSCF:5060 + S-CSCF:5070 + IMS-HSS:3870)"
# tee: output goes to BOTH terminal (live) AND log file simultaneously.
# This avoids the buffering issue where redirecting to a file silently
# holds output in an 8 KB buffer that never flushes if the process is killed.
LOG_LEVEL="$LOG_LEVEL" ./ims_server < "$SERVER_PIPE" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
exec 3>"$SERVER_PIPE"
wait_s 2

# ── Start UE-A ───────────────────────────────────────────────
banner "Step 2 — UE-A (caller, IP 10.0.0.1, RTP:50000)"
./ue_sim A < "$UE_A_PIPE" > "$UE_A_LOG" 2>&1 &
UE_A_PID=$!
exec 4>"$UE_A_PIPE"
wait_s 1

# ── Start UE-B ───────────────────────────────────────────────
banner "Step 3 — UE-B (callee, IP 10.0.0.2, RTP:60000)"
./ue_sim B < "$UE_B_PIPE" > "$UE_B_LOG" 2>&1 &
UE_B_PID=$!
exec 5>"$UE_B_PIPE"
wait_s 1

# ── IMS REGISTER ─────────────────────────────────────────────
banner "Step 4 — SIP REGISTER  (UE-A)"
step "REG → REGISTER → Cx SAR/SAA → 200 OK"
echo "REG" >&4
wait_s $DELAY

banner "Step 5 — SIP REGISTER  (UE-B)"
step "REG → REGISTER → Cx SAR/SAA → 200 OK"
echo "REG" >&5
wait_s $DELAY

step "STATUS — show registered UEs"
echo "STATUS" >&3
wait_s 1

# ── VoLTE CALL SETUP ─────────────────────────────────────────
banner "Step 6 — VoLTE INVITE  (UE-A → UE-B)"
step "CALL B → INVITE → 100 Trying → 183 Session Progress"
step "       → PRACK → 200 OK PRACK → 180 Ringing"
echo "CALL B" >&4
wait_s $DELAY

banner "Step 7 — UE-B answers"
step "ACCEPT → 200 OK (SDP: AMR-WB/16000) → ACK → QCI=1 bearer"
echo "ACCEPT" >&5
wait_s $DELAY

# ── HOLD ─────────────────────────────────────────────────────
banner "Step 8 — HOLD  (re-INVITE a=sendonly)"
step "HOLD → re-INVITE CSeq:2 a=sendonly → 200 OK"
echo "HOLD" >&4
wait_s $DELAY

# ── RESUME ───────────────────────────────────────────────────
banner "Step 9 — RESUME  (re-INVITE a=sendrecv)"
step "RESUME → re-INVITE CSeq:3 a=sendrecv → 200 OK"
echo "RESUME" >&4
wait_s $DELAY

# ── BYE ──────────────────────────────────────────────────────
banner "Step 10 — BYE  (teardown)"
step "BYE → S-CSCF closes CDR → Rx STR → PCRF → QCI=1 released → 200 OK BYE"
echo "BYE" >&4
wait_s $DELAY

# ── Shutdown ─────────────────────────────────────────────────
banner "Step 11 — Shutdown"
echo "QUIT" >&4
echo "QUIT" >&5
wait_s 2
echo "QUIT" >&3

exec 3>&- 4>&- 5>&-
wait "$UE_A_PID"   2>/dev/null || true
wait "$UE_B_PID"   2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
echo -e "${GREEN}  All processes exited — logs flushed.${RESET}"

# ── Merge pcaps ──────────────────────────────────────────────
banner "Step 12 — Merging pcap files"
if [ -f ./merge_pcap.sh ]; then
    bash ./merge_pcap.sh
fi

# ── Summary ──────────────────────────────────────────────────
banner "DONE"
echo ""
echo -e "${GREEN}  Files:${RESET}"
ls -lh "$SERVER_LOG" "$UE_A_LOG" "$UE_B_LOG" ims_combined.pcap 2>/dev/null
echo ""
echo -e "${YELLOW}  Wireshark filters:${RESET}"
echo "    sip                          → all SIP"
echo "    sip.Method == \"REGISTER\"     → registration"
echo "    sip.Method == \"INVITE\"       → call setup + re-INVITEs"
echo "    sip.Method == \"PRACK\"        → reliable provisional (100rel)"
echo "    sip.Status-Code == 100       → 100 Trying"
echo "    sip.Status-Code == 180       → 180 Ringing"
echo "    sip.Status-Code == 183       → 183 Session Progress (QoS)"
echo "    sip.Status-Code == 200       → 200 OK (REG/PRACK/INVITE/BYE)"
echo "    sip.Method == \"BYE\"         → teardown"
echo "    diameter                     → Cx SAR/SAA  Rx AAR/AAA"
echo "    ip.addr == 10.0.0.1          → UE-A only"
echo "    ip.addr == 10.0.0.2          → UE-B only"
echo "    ip.addr == 10.0.0.8          → P-CSCF only"
echo "    ip.addr == 10.0.0.9          → S-CSCF only"
echo ""
echo -e "${GREEN}  Packet sequence in ims_combined.pcap:${RESET}"
echo ""
echo "  REG (A) → 200 OK REGISTER"
echo "  REG (B) → 200 OK REGISTER"
echo "  INVITE → 100 Trying → 183 Session Progress (SDP+QoS)"
echo "         → PRACK → 200 OK PRACK"
echo "         → 180 Ringing → 200 OK INVITE (SDP answer) → ACK"
echo "  re-INVITE (a=sendonly HOLD) → 200 OK"
echo "  re-INVITE (a=sendrecv RESUME) → 200 OK"
echo "  BYE → 200 OK BYE"
echo "  Diameter Cx SAR/SAA"
echo ""

# ── Server log tail ───────────────────────────────────────────
banner "Server log (last 50 lines)"
tail -50 "$SERVER_LOG"

# ── Auto-open web visualizer ──────────────────────────────────
VIZPATH="$(dirname "$0")/volte_call_flow.html"
if [ -f "$VIZPATH" ]; then
  echo ""
  echo -e "${CYAN}  Opening call flow visualizer in browser...${RESET}"
  echo -e "${YELLOW}  File: $VIZPATH${RESET}"
  echo -e "  Press ▶ Play (or Space) to animate the full VoLTE flow."
  open "$VIZPATH" 2>/dev/null || xdg-open "$VIZPATH" 2>/dev/null || echo "  → Open manually: $VIZPATH"
fi
