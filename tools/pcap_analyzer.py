#!/usr/bin/env python3
# pcap_analyzer.py — Rule-based PCAP fault diagnosis for the telecom simulator.
# Reads a .pcap file using pyshark and applies protocol-aware rules to detect
# common failures (auth failure, timeout, SIP 4xx, PFCP errors).
# Usage: python3 tools/pcap_analyzer.py mme_capture.pcap
#        python3 tools/pcap_analyzer.py 5g_smf_capture.pcap
# Optional: --ai for natural language explanation via Ollama (if available)

import argparse
import pathlib
import sys
import json
import re
import time

# ── Colors ─────────────────────────────────────────────────────────────────────
R  = '\033[0m'
B  = '\033[1m'
GR = '\033[1;32m'
YE = '\033[1;33m'
RD = '\033[1;31m'
CY = '\033[1;36m'
BL = '\033[1;34m'

def ok(msg):   print(f"  {GR}✓{R}  {msg}")
def warn(msg): print(f"  {YE}⚠{R}  {msg}")
def err(msg):  print(f"  {RD}✗{R}  {msg}")
def info(msg): print(f"  {CY}ℹ{R}  {msg}")

# ── Import pyshark ─────────────────────────────────────────────────────────────
try:
    import pyshark
    HAS_PYSHARK = True
except ImportError:
    HAS_PYSHARK = False

# ── Fallback: read raw pcap using struct (no dependencies) ────────────────────
import struct

def read_pcap_raw(path: pathlib.Path) -> list[dict]:
    """Minimal pcap global header + packet reader — no pyshark needed."""
    packets = []
    try:
        with open(path, "rb") as f:
            magic = struct.unpack("<I", f.read(4))[0]
            if magic not in (0xA1B2C3D4, 0xD4C3B2A1):
                return packets
            f.read(20)  # skip rest of global header
            while True:
                hdr = f.read(16)
                if len(hdr) < 16: break
                ts_sec, ts_usec, incl_len, orig_len = struct.unpack("<IIII", hdr)
                data = f.read(incl_len)
                packets.append({"ts": ts_sec + ts_usec/1e6, "data": data,
                                 "incl_len": incl_len, "orig_len": orig_len})
    except Exception as e:
        err(f"Could not read pcap: {e}")
    return packets

def extract_text_heuristic(data: bytes) -> str:
    """Try to extract readable ASCII from packet payload."""
    try:
        text = data[42:].decode("ascii", errors="replace")  # skip Eth+IP+TCP/UDP
    except Exception:
        text = ""
    return text

# ── Rules ─────────────────────────────────────────────────────────────────────
class Finding:
    def __init__(self, severity: str, title: str, detail: str,
                 packet_num: int = 0, fix: str = ""):
        self.severity   = severity  # OK | WARN | ERR
        self.title      = title
        self.detail     = detail
        self.packet_num = packet_num
        self.fix        = fix

    def print(self):
        prefix = {
            "OK":   (GR, "✓"),
            "WARN": (YE, "⚠"),
            "ERR":  (RD, "✗"),
            "INFO": (CY, "ℹ"),
        }.get(self.severity, (R, "·"))
        c, sym = prefix
        loc = f" [pkt #{self.packet_num}]" if self.packet_num else ""
        print(f"  {c}{sym}{R}  {B}{self.title}{R}{loc}")
        print(f"       {self.detail}")
        if self.fix:
            print(f"       {CY}FIX: {self.fix}{R}")

def analyze_with_pyshark(pcap_path: pathlib.Path) -> list[Finding]:
    findings = []
    cap = pyshark.FileCapture(str(pcap_path), keep_packets=False)

    diameters_seen  = set()
    sip_responses   = []
    auth_req_ts     = None
    auth_rsp_ts     = None
    gtp_reqs        = {}   # seq → ts
    pfcp_reqs       = {}   # seq → ts

    pkt_num = 0
    try:
        for pkt in cap:
            pkt_num += 1
            layers = {l.layer_name for l in pkt.layers}

            # ── Diameter ──────────────────────────────────────────────
            if "diameter" in layers:
                d = pkt.diameter
                cc = getattr(d, "cmd_code", "")
                is_req = getattr(d, "flags_request", "0") == "1"
                diameters_seen.add(cc)
                if cc == "318" and is_req:       # AIR
                    auth_req_ts = float(pkt.sniff_timestamp)
                elif cc == "318" and not is_req:  # AIA
                    auth_rsp_ts = float(pkt.sniff_timestamp)
                    result = getattr(d, "avp_result_code", "2001")
                    if result != "2001":
                        findings.append(Finding("ERR",
                            "Diameter AIR failed",
                            f"AIA Result-Code = {result} (expected 2001 DIAMETER_SUCCESS). "
                            f"Likely cause: IMSI not provisioned in HSS or wrong Ki/OPc.",
                            pkt_num,
                            "Check HSS subscriber database. Verify IMSI and Ki match."))

            # ── GTPv2 ─────────────────────────────────────────────────
            if "gtpv2" in layers:
                g = pkt.gtpv2
                msg_type = getattr(g, "message_type", "0")
                seq      = getattr(g, "seq", "0")
                if msg_type == "32":   # CSReq
                    gtp_reqs[seq] = float(pkt.sniff_timestamp)
                elif msg_type == "33": # CSRsp
                    cause = getattr(g, "cause", "16")
                    if cause != "16":  # 16 = Request Accepted
                        findings.append(Finding("ERR",
                            "GTPv2 Create Session failed",
                            f"Create Session Response cause = {cause} (expected 16 = accepted). "
                            f"SGW/PGW rejected the session.",
                            pkt_num,
                            "Check SGW/PGW reachability and APN configuration."))
                    if seq in gtp_reqs:
                        delta = float(pkt.sniff_timestamp) - gtp_reqs[seq]
                        if delta > 2.0:
                            findings.append(Finding("WARN",
                                f"GTPv2 Create Session slow ({delta:.2f}s)",
                                "Create Session Response took >2s. PGW or PCRF may be overloaded.",
                                pkt_num,
                                "Check PGW/PCRF logs. Ensure Gx interface is up."))

            # ── SIP ───────────────────────────────────────────────────
            if "sip" in layers:
                s   = pkt.sip
                rsp = getattr(s, "Status_Code", "") or getattr(s, "status_code", "")
                if rsp:
                    sip_responses.append((pkt_num, rsp))
                    if rsp.startswith("4") or rsp.startswith("5"):
                        reason = getattr(s, "reason_phrase", getattr(s, "Status_Line", ""))
                        findings.append(Finding("ERR",
                            f"SIP error response: {rsp}",
                            f"SIP {rsp} {reason} at packet #{pkt_num}.",
                            pkt_num,
                            {
                                "401": "Normal Digest challenge — check if 200 OK follows.",
                                "403": "Authorization failed — check IMS-AKA credentials.",
                                "404": "User not found — check IMPU routing in S-CSCF.",
                                "488": "Codec mismatch — verify AMR-WB in SDP offer/answer (IR.92).",
                                "486": "UE B busy — check call waiting config in MTAS.",
                                "503": "IMS server overloaded — check S-CSCF/MTAS capacity.",
                            }.get(rsp, "Check IMS trace log and Cx interface to HSS.")))

            # ── PFCP ─────────────────────────────────────────────────
            if "pfcp" in layers:
                p = pkt.pfcp
                msg_type = getattr(p, "message_type", "0")
                seq      = getattr(p, "sequence_number", "0")
                if msg_type == "50":  # Est Req
                    pfcp_reqs[seq] = float(pkt.sniff_timestamp)
                elif msg_type == "51":  # Est Rsp
                    cause = getattr(p, "cause_value", "1")
                    if cause != "1":  # 1 = Request Accepted
                        findings.append(Finding("ERR",
                            f"PFCP Session Establishment failed (cause={cause})",
                            "UPF rejected the PFCP session. PDU session cannot be established.",
                            pkt_num,
                            "Check UPF logs. Verify N4 IP reachability (SMF↔UPF)."))

        cap.close()
    except Exception as e:
        findings.append(Finding("WARN", "pyshark parse error",
                                f"Could not fully parse pcap: {e}. Partial results shown."))

    # ── Auth timeout check ─────────────────────────────────────────
    if auth_req_ts is not None and auth_rsp_ts is None:
        findings.append(Finding("ERR",
            "Diameter AIR sent but no AIA received",
            "HSS did not respond to Authentication-Information-Request. "
            "Possible: HSS unreachable, S6a interface down, SCTP association lost.",
            0,
            "Check HSS connectivity. Verify Diameter peer config and firewall rules."))
    elif auth_req_ts and auth_rsp_ts:
        delta = auth_rsp_ts - auth_req_ts
        if delta > 1.0:
            findings.append(Finding("WARN",
                f"Diameter AIR→AIA latency high ({delta:.3f}s)",
                "HSS responded slowly. Under load, HSS auth delay causes Attach timeouts.",
                0,
                "Check HSS database query times. Consider HSS caching or load balancing."))
        else:
            findings.append(Finding("OK",
                f"Diameter S6a auth OK (latency={delta:.3f}s)",
                "AIR→AIA round trip within normal range."))

    return findings

def analyze_without_pyshark(pcap_path: pathlib.Path) -> list[Finding]:
    """Lightweight analysis using raw packet reading — no pyshark."""
    findings = []
    packets = read_pcap_raw(pcap_path)
    if not packets:
        findings.append(Finding("WARN", "Empty or unreadable PCAP",
                                "No packets found. Check file path and format."))
        return findings

    info_line = f"PCAP contains {len(packets)} packets, " \
                f"{sum(p['orig_len'] for p in packets)} bytes total"
    findings.append(Finding("INFO", "PCAP summary", info_line))

    sip_errors  = []
    has_diameter = False
    has_gtp      = False
    has_pfcp     = False
    has_sip      = False

    for i, pkt in enumerate(packets):
        text = extract_text_heuristic(pkt["data"])
        # SIP detection
        if "SIP/2.0 4" in text or "SIP/2.0 5" in text:
            has_sip = True
            m = re.search(r"SIP/2\.0 (\d{3})[^\r\n]*", text)
            if m:
                sip_errors.append((i + 1, m.group(1)))
        if "SIP/2.0" in text:
            has_sip = True
        # Protocol markers in hex (just presence detection)
        data = pkt["data"]
        if len(data) > 42 and data[9:10] == b"\x11":  # UDP
            if len(data) > 50:
                dst_port = struct.unpack(">H", data[36:38])[0]
                if dst_port in (3868, 3869): has_diameter = True
                if dst_port in (2123, 2125): has_gtp = True
                if dst_port == 8805:          has_pfcp = True

    if has_diameter:
        findings.append(Finding("INFO", "Diameter packets detected",
                                "S6a/Cx/Gx Diameter messages present. "
                                "Install pyshark+tshark for detailed field analysis."))
    if has_gtp:
        findings.append(Finding("INFO", "GTPv2 packets detected",
                                "GTP-C (port 2123/2125) messages present."))
    if has_pfcp:
        findings.append(Finding("INFO", "PFCP packets detected",
                                "PFCP (N4, port 8805) messages present."))
    if has_sip:
        findings.append(Finding("INFO", "SIP packets detected",
                                "IMS/VoLTE SIP messages present."))
    for pkt_num, code in sip_errors:
        findings.append(Finding("ERR" if not code.startswith("401") else "WARN",
            f"SIP {code} response at packet #{pkt_num}",
            {
                "401": "Digest auth challenge — normal if followed by re-REGISTER.",
                "403": "Forbidden — check IMS-AKA credentials.",
                "404": "Not found — IMPU not registered.",
                "488": "Not Acceptable — codec mismatch in SDP. Check AMR-WB (IR.92).",
            }.get(code, f"SIP error {code}."),
            pkt_num))

    return findings

# ── AI explanation (optional Ollama) ─────────────────────────────────────────
def ai_explain(findings: list[Finding], pcap_path: pathlib.Path):
    try:
        import urllib.request, urllib.error
        summary = "\n".join(f"[{f.severity}] {f.title}: {f.detail}" for f in findings)
        payload = json.dumps({
            "model": "llama3",
            "prompt": f"This is a telecom network PCAP analysis result from {pcap_path.name}. "
                      f"Summarize the findings in 3-5 sentences for a network engineer:\n\n{summary}",
            "stream": False
        }).encode()
        req = urllib.request.Request("http://localhost:11434/api/generate",
                                      data=payload, method="POST",
                                      headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read())
            print(f"\n{BL}AI Explanation (Ollama/llama3):{R}")
            print(f"  {data.get('response', 'No response')}")
    except Exception as e:
        warn(f"AI explanation unavailable: {e}. Start Ollama with 'ollama run llama3'.")

# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Rule-based PCAP fault diagnosis for 4G/5G/IMS simulator captures",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Detects:
  • Diameter AIR timeout (HSS unreachable)
  • Diameter AIA auth failure (wrong Ki/OPc)
  • GTPv2 Create Session rejected
  • SIP 4xx/5xx errors (codec mismatch, not found, forbidden)
  • PFCP Session Establishment failure

Examples:
  python3 tools/pcap_analyzer.py 4g-simulator/mme_capture.pcap
  python3 tools/pcap_analyzer.py 5g-simulator/5g_smf_capture.pcap
  python3 tools/pcap_analyzer.py ims-simulator/ims_capture.pcap --ai
""")
    parser.add_argument("pcap", help="Path to .pcap file to analyze")
    parser.add_argument("--ai",  action="store_true",
                        help="Send summary to local Ollama (llama3) for NL explanation")
    args = parser.parse_args()

    pcap_path = pathlib.Path(args.pcap)
    if not pcap_path.exists():
        pcap_path = pathlib.Path.cwd() / args.pcap
    if not pcap_path.exists():
        print(f"ERROR: PCAP file not found: {args.pcap}")
        sys.exit(1)

    print(f"\n{B}{'─'*60}{R}")
    print(f"{B}PCAP Fault Analyzer — {pcap_path.name}{R}")
    print(f"{'─'*60}")
    print(f"  File:  {pcap_path.resolve()}")
    print(f"  Size:  {pcap_path.stat().st_size:,} bytes")
    print(f"  Engine: {'pyshark (deep decode)' if HAS_PYSHARK else 'raw pcap (install pyshark+tshark for deep decode)'}")
    print()

    t0 = time.time()
    if HAS_PYSHARK:
        findings = analyze_with_pyshark(pcap_path)
    else:
        findings = analyze_without_pyshark(pcap_path)
    elapsed = time.time() - t0

    print(f"{B}Findings ({len(findings)} total, {elapsed:.1f}s):{R}\n")
    for f in findings:
        f.print()
        print()

    errors = [f for f in findings if f.severity == "ERR"]
    warns  = [f for f in findings if f.severity == "WARN"]
    print(f"{'─'*60}")
    if errors:
        print(f"{RD}VERDICT: {len(errors)} error(s) found — review findings above{R}")
    elif warns:
        print(f"{YE}VERDICT: {len(warns)} warning(s) — check details above{R}")
    else:
        print(f"{GR}VERDICT: No errors detected{R}")

    if not HAS_PYSHARK:
        print(f"\n{CY}TIP: Install pyshark+tshark for full protocol decode:{R}")
        print(f"     pip install pyshark")
        print(f"     brew install wireshark  (macOS)  or  apt install tshark  (Linux)")

    if args.ai:
        ai_explain(findings, pcap_path)

if __name__ == "__main__":
    main()
