#pragma once
// ============================================================
// S1AP (TS 36.413) PDU builders — produce real ASN.1 Aligned-PER
// encoded byte sequences so Wireshark's native "s1ap" dissector
// (registered on SCTP PPID 18) decodes our pcap exactly like a
// real eNB<->MME capture.
//
// Approach: byte-template + splice. Each PDU type below is a
// constant byte template (generated + verified via pycrate; see
// project memory s1ap_aper_plan.md) with a handful of fixed-width
// variable fields overwritten at known offsets.
//
// Limitation: ENB-UE-S1AP-ID and MME-UE-S1AP-ID splices only
// support values 0-255 (sufficient for this demo's UE counts).
// ============================================================
#include <cstdint>
#include <vector>

namespace s1ap {

// InitialUEMessage: Attach Request + PDN Connectivity Request (66 bytes).
std::vector<uint8_t> buildInitialUEMessage(uint32_t enb_ue_id, uint64_t imsi);

// DownlinkNASTransport / UplinkNASTransport — generic 3-IE wrapper
// (MME-UE-S1AP-ID, ENB-UE-S1AP-ID, NAS-PDU) around any NAS-EPS message.
std::vector<uint8_t> buildDlNasTransport(uint32_t mme_id, uint32_t enb_id,
                                          const std::vector<uint8_t>& nas);
std::vector<uint8_t> buildUlNasTransport(uint32_t mme_id, uint32_t enb_id,
                                          const std::vector<uint8_t>& nas);

// InitialContextSetupRequest: bearer setup (S-GW TEID, KeNB, UE security caps)
// + embedded NAS Attach Accept / Activate Default EPS Bearer Context Request.
std::vector<uint8_t> buildInitialContextSetupRequest(uint32_t mme_id, uint32_t enb_id,
                                                       uint32_t sgw_s1u_teid,
                                                       const uint8_t ue_ip[4],
                                                       const uint8_t kenb32[32]);

// InitialContextSetupResponse: eNB's S1-U TEID for the bearer.
std::vector<uint8_t> buildInitialContextSetupResponse(uint32_t mme_id, uint32_t enb_id,
                                                        uint32_t enb_s1u_teid);

} // namespace s1ap
