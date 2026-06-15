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

} // namespace s1ap
