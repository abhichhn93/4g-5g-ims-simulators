#include "common/s1ap_codec.h"
#include "common/nas_eps.h"
#include <cstdio>

namespace s1ap {

// Convert a hex-string template (no spaces/separators) into bytes.
static std::vector<uint8_t> hex2bytes(const char* hex) {
    std::vector<uint8_t> out;
    for (const char* p = hex; p[0] && p[1]; p += 2) {
        unsigned byte;
        std::sscanf(p, "%2x", &byte);
        out.push_back(static_cast<uint8_t>(byte));
    }
    return out;
}

// InitialUEMessage: Attach Request + PDN Connectivity Request.
// Template captured for enb_ue_id=1, imsi=404100000000001 (66 bytes).
//   offset 12       : ENB-UE-S1AP-ID (0-255)
//   offsets 22-29   : EPS Mobile Identity (IMSI, 8-byte BCD)
std::vector<uint8_t> buildInitialUEMessage(uint32_t enb_ue_id, uint64_t imsi) {
    auto pdu = hex2bytes(
        "000c403e000005000800020001001a00161507417108494001000000001002e0e0"
        "00040201d011004300060004f4010001006400080004f401000000100086400130");
    pdu[12] = static_cast<uint8_t>(enb_ue_id & 0xFF);
    auto epsid = nas_eps::encodeEpsIdImsi(imsi);
    for (size_t i = 0; i < epsid.size(); ++i) pdu[22 + i] = epsid[i];
    return pdu;
}

// Generic DownlinkNASTransport / UplinkNASTransport wrapper around a NAS-EPS PDU.
// 24-byte prefix template (mme_id=0, enb_id=1) + appended NAS-PDU bytes.
//   idx0-1  : procedure code (0x000b=Downlink, 0x000d=Uplink)
//   idx3    : 20 + nas.size()  (overall PDU length, valid for nas.size() < 128)
//   idx12   : MME-UE-S1AP-ID (0-255)
//   idx18   : ENB-UE-S1AP-ID (0-255)
//   idx22   : nas.size() + 1  (NAS-PDU open-type length)
//   idx23   : nas.size()      (NAS-PDU octet-string length)
static std::vector<uint8_t> buildNasTransport(uint8_t proc0, uint8_t proc1,
                                               uint32_t mme_id, uint32_t enb_id,
                                               const std::vector<uint8_t>& nas) {
    auto pdu = hex2bytes(
        "000b4038000003000000020000000800020001001a002524");
    pdu[0] = proc0;
    pdu[1] = proc1;
    pdu[3] = static_cast<uint8_t>(20 + nas.size());
    pdu[12] = static_cast<uint8_t>(mme_id & 0xFF);
    pdu[18] = static_cast<uint8_t>(enb_id & 0xFF);
    pdu[22] = static_cast<uint8_t>(nas.size() + 1);
    pdu[23] = static_cast<uint8_t>(nas.size());
    pdu.insert(pdu.end(), nas.begin(), nas.end());
    return pdu;
}

std::vector<uint8_t> buildDlNasTransport(uint32_t mme_id, uint32_t enb_id,
                                          const std::vector<uint8_t>& nas) {
    return buildNasTransport(0x00, 0x0b, mme_id, enb_id, nas);
}

std::vector<uint8_t> buildUlNasTransport(uint32_t mme_id, uint32_t enb_id,
                                          const std::vector<uint8_t>& nas) {
    return buildNasTransport(0x00, 0x0d, mme_id, enb_id, nas);
}

// InitialContextSetupRequest: bearer setup + embedded Attach Accept NAS-PDU.
// Template captured for mme_id=0, enb_id=1, sgw_ip=10.0.0.5, sgw_teid=0x12345678,
// kenb=0x11x32, ue_ip=10.45.0.2 (137 bytes).
//   offset 13       : MME-UE-S1AP-ID (0-255)
//   offset 19       : ENB-UE-S1AP-ID (0-255)
//   offsets 35-38   : S-GW Transport Layer Address (IPv4, constant = IP_SGW)
//   offsets 39-42   : S-GW S1-U TEID (big-endian)
//   offsets 74-77   : UE PDN IPv4 address (inside embedded Attach Accept)
//   offsets 96-127  : KeNB / SecurityKey (32 bytes)
//   offsets 132,134 : encryption/integrity algorithm bitfields (kept constant)
std::vector<uint8_t> buildInitialContextSetupRequest(uint32_t mme_id, uint32_t enb_id,
                                                       uint32_t sgw_s1u_teid,
                                                       const uint8_t ue_ip[4],
                                                       const uint8_t kenb32[32]) {
    auto pdu = hex2bytes(
        "00090080840000060000000200000008000200010018003600003440314500093c0f80"
        "0a00000512345678220742011e060004f401000100155201c101090908696e7465726e"
        "657405010a2d00020042000a1805f5e1006002faf08000490020111111111111111111"
        "1111111111111111111111111111111111111111111111006b00051c000e0000");
    pdu[13] = static_cast<uint8_t>(mme_id & 0xFF);
    pdu[19] = static_cast<uint8_t>(enb_id & 0xFF);
    pdu[39] = static_cast<uint8_t>((sgw_s1u_teid >> 24) & 0xFF);
    pdu[40] = static_cast<uint8_t>((sgw_s1u_teid >> 16) & 0xFF);
    pdu[41] = static_cast<uint8_t>((sgw_s1u_teid >> 8) & 0xFF);
    pdu[42] = static_cast<uint8_t>(sgw_s1u_teid & 0xFF);
    for (int i = 0; i < 4; ++i) pdu[74 + i] = ue_ip[i];
    for (int i = 0; i < 32; ++i) pdu[96 + i] = kenb32[i];
    return pdu;
}

// InitialContextSetupResponse: eNB's S1-U TEID for the bearer.
// Template captured for mme_id=0, enb_id=1, enb_teid=0x87654321 (38 bytes).
//   offset 12     : MME-UE-S1AP-ID (0-255)
//   offset 18     : ENB-UE-S1AP-ID (0-255)
//   offset 33     : eNB Transport Layer Address last octet (constant = IP_ENB)
//   offsets 34-37 : eNB S1-U TEID (big-endian)
std::vector<uint8_t> buildInitialContextSetupResponse(uint32_t mme_id, uint32_t enb_id,
                                                        uint32_t enb_s1u_teid) {
    auto pdu = hex2bytes(
        "200900220000030000000200000008000200010033400f000032400a0a1f0a00000287654321");
    pdu[12] = static_cast<uint8_t>(mme_id & 0xFF);
    pdu[18] = static_cast<uint8_t>(enb_id & 0xFF);
    pdu[34] = static_cast<uint8_t>((enb_s1u_teid >> 24) & 0xFF);
    pdu[35] = static_cast<uint8_t>((enb_s1u_teid >> 16) & 0xFF);
    pdu[36] = static_cast<uint8_t>((enb_s1u_teid >> 8) & 0xFF);
    pdu[37] = static_cast<uint8_t>(enb_s1u_teid & 0xFF);
    return pdu;
}

// ── Handover S1AP PDU helpers ─────────────────────────────────
//
// S1AP APER outer frame structure (derived from existing templates):
//   Byte 0: PDU type (0x00=initiatingMessage, 0x20=successfulOutcome)
//   Byte 1: Procedure code (see TS 36.413 §9.1 for procedure list)
//   Byte 2: Criticality (0x00=reject, 0x40=ignore, 0x80=notify)
//   Byte 3: Value length (byte count of remaining body)
//   Byte 4: Extension marker (0x00 = no extensions present)
//   Byte 5-6: IE count big-endian (e.g. 0x00 0x02 = 2 IEs)
//
// Each IE:
//   2B: IE ID (big-endian, from TS 36.413 §9.2)
//   1B: criticality (0x00=reject, 0x40=ignore, 0x80=notify)
//   1B: open-type length (value bytes that follow)
//   NB: value
//
// IE IDs used here:
//   id-MME-UE-S1AP-ID = 0 (0x0000), id-eNB-UE-S1AP-ID = 8 (0x0008)
//   id-HandoverType = 1 (0x0001), id-Cause = 2 (0x0002)
//   id-UE-S1AP-IDs = 99 (0x0063)
//
// INTERVIEW NOTE: TS 36.413 §8.4 defines the S1 Handover procedure.
// It's a 7-step flow: Required→Request→Ack→Command→StatusXfer→Notify→Release.

namespace {
// Build a minimal but structurally valid S1AP frame for handover messages.
// 2 primary IEs: MME-UE-S1AP-ID and ENB-UE-S1AP-ID.
static std::vector<uint8_t> buildHoPdu(uint8_t pdu_type, uint8_t proc_code,
                                        uint8_t criticality,
                                        uint32_t mme_id, uint32_t enb_id,
                                        const std::vector<uint8_t>& extra_ies = {}) {
    // IEs: [2B ID][1B crit][1B len][NB val]
    // MME-UE-S1AP-ID (id=0, reject=0x00, 2-byte value)
    std::vector<uint8_t> ies = {
        0x00, 0x00,  0x00,  0x02,
        static_cast<uint8_t>((mme_id >> 8) & 0xFF), static_cast<uint8_t>(mme_id & 0xFF),
        // ENB-UE-S1AP-ID (id=8, reject=0x00, 2-byte value)
        0x00, 0x08,  0x00,  0x02,
        static_cast<uint8_t>((enb_id >> 8) & 0xFF), static_cast<uint8_t>(enb_id & 0xFF),
    };
    ies.insert(ies.end(), extra_ies.begin(), extra_ies.end());

    uint8_t n_ies = uint8_t(2 + extra_ies.size() / 6); // each extra IE is ~6 bytes
    // Body: [ext=0x00][count 2B][IEs]
    std::vector<uint8_t> body = {0x00, 0x00, n_ies};
    body.insert(body.end(), ies.begin(), ies.end());

    std::vector<uint8_t> pdu = {pdu_type, proc_code, criticality,
                                  static_cast<uint8_t>(body.size())};
    pdu.insert(pdu.end(), body.begin(), body.end());
    return pdu;
}
} // anonymous namespace

std::vector<uint8_t> buildHandoverRequired(uint32_t mme_id, uint32_t enb_id) {
    // proc=0 (HandoverPreparation), initiating, reject
    // Extra IE: HandoverType (id=1, reject, 1B: intralte=0)
    std::vector<uint8_t> extra = {0x00, 0x01, 0x00, 0x01, 0x00,  // HandoverType=intralte
                                   0x00, 0x02, 0x40, 0x02, 0x00, 0x00}; // Cause=radioNetwork.0
    return buildHoPdu(0x00, 0x00, 0x00, mme_id, enb_id, extra);
}

std::vector<uint8_t> buildHandoverRequest(uint32_t mme_id, uint32_t enb_id) {
    // proc=1 (HandoverResourceAllocation), initiating, reject
    std::vector<uint8_t> extra = {0x00, 0x01, 0x00, 0x01, 0x00,  // HandoverType=intralte
                                   0x00, 0x02, 0x40, 0x02, 0x00, 0x00}; // Cause=radioNetwork.0
    return buildHoPdu(0x00, 0x01, 0x00, mme_id, enb_id, extra);
}

std::vector<uint8_t> buildHandoverRequestAck(uint32_t mme_id, uint32_t enb_id) {
    // proc=1, successfulOutcome, ignore
    return buildHoPdu(0x20, 0x01, 0x40, mme_id, enb_id);
}

std::vector<uint8_t> buildHandoverCommand(uint32_t mme_id, uint32_t enb_id) {
    // proc=0, successfulOutcome (HandoverCommand is a successful outcome of HandoverPreparation)
    return buildHoPdu(0x20, 0x00, 0x00, mme_id, enb_id);
}

std::vector<uint8_t> buildENBStatusTransfer(uint32_t mme_id, uint32_t enb_id) {
    // proc=24 (0x18), initiating, ignore
    return buildHoPdu(0x00, 0x18, 0x40, mme_id, enb_id);
}

std::vector<uint8_t> buildMMEStatusTransfer(uint32_t mme_id, uint32_t enb_id) {
    // proc=25 (0x19), initiating, ignore
    return buildHoPdu(0x00, 0x19, 0x40, mme_id, enb_id);
}

std::vector<uint8_t> buildHandoverNotify(uint32_t mme_id, uint32_t enb_id) {
    // proc=2, initiating, ignore
    return buildHoPdu(0x00, 0x02, 0x40, mme_id, enb_id);
}

std::vector<uint8_t> buildUEContextReleaseCommand(uint32_t mme_id, uint32_t enb_id) {
    // proc=23 (0x17), initiating, reject
    return buildHoPdu(0x00, 0x17, 0x00, mme_id, enb_id);
}

std::vector<uint8_t> buildUEContextReleaseComplete(uint32_t mme_id, uint32_t enb_id) {
    // proc=23 (0x17), successfulOutcome, ignore
    return buildHoPdu(0x20, 0x17, 0x40, mme_id, enb_id);
}

} // namespace s1ap
