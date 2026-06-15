#!/bin/bash
# ============================================================
# Merge all IMS PCAP files into one — open in Wireshark
# Run this AFTER doing REG + CALL in all UE terminals
#
# IPs in the merged PCAP (to differentiate traffic):
#   127.0.0.1 = UE-A     127.0.0.2 = UE-B     127.0.0.3 = UE-C
#   127.0.0.8 = P-CSCF   127.0.0.9 = S-CSCF   127.0.0.4 = IMS-HSS
#
# Wireshark filters:
#   ip.addr == 127.0.0.1   → UE-A traffic only
#   ip.addr == 127.0.0.2   → UE-B traffic only
#   ip.src == 127.0.0.8    → P-CSCF sent
#   ip.src == 127.0.0.9    → S-CSCF sent
#   tcp.port == 5060       → all SIP
# ============================================================

BUILD="$(dirname "$0")/build"
OUT="$BUILD/ims_combined.pcap"

FILES=""
[ -f "$BUILD/ims_A_capture.pcap"      ] && FILES="$FILES $BUILD/ims_A_capture.pcap"
[ -f "$BUILD/ims_B_capture.pcap"      ] && FILES="$FILES $BUILD/ims_B_capture.pcap"
[ -f "$BUILD/ims_C_capture.pcap"      ] && FILES="$FILES $BUILD/ims_C_capture.pcap"
[ -f "$BUILD/ims_server_capture.pcap" ] && FILES="$FILES $BUILD/ims_server_capture.pcap"

if [ -z "$FILES" ]; then
    echo "No PCAP files found. Run ims_server + ue_sim A/B first."
    exit 1
fi

echo "Merging: $FILES"
mergecap -w "$OUT" $FILES
echo ""
echo "Combined PCAP: $OUT"
echo ""
echo "Opening in Wireshark..."
echo ""
echo "Useful filters:"
echo "  ip.addr == 127.0.0.1          → UE-A"
echo "  ip.addr == 127.0.0.2          → UE-B"
echo "  ip.addr == 127.0.0.8          → P-CSCF"
echo "  ip.addr == 127.0.0.9          → S-CSCF"
echo "  tcp.port == 5060              → all SIP (REGISTER, INVITE, etc.)"
echo "  mme_sim2                      → decoded messages (needs Lua dissector)"
echo ""
open "$OUT"
