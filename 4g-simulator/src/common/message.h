#pragma once
#include <cstdint>
#include <cstring>
#include <ostream>
#include <cstdio>
#include "common/message_types.h"

// ============================================================
// WIRE FORMAT STRUCTS
//
// #pragma pack(push,1): compiler inserts NO padding bytes between fields.
// Without this, the compiler adds padding to align fields to their natural
// alignment (e.g., uint32_t aligned to 4 bytes). With packing, the struct
// layout matches exactly what goes on the wire.
//
// BYTE ORDER: Using host byte order (little-endian on x86/ARM64) for Phase 1.
// Both nodes are on the same host (127.0.0.1) so this works.
// Real systems use network byte order (big-endian): htonl(), ntohl().
// Phase 2: add proper byte-order conversion at send/recv boundaries.
//
// REAL S1AP ENCODING: ASN.1 SEQUENCE with Aligned PER (APER) encoding.
// Each IE is wrapped in:
//   ProtocolIE-Container { id INTEGER, criticality ENUMERATED, value OPEN TYPE }
//   criticality: reject | ignore | notify (what to do if IE is unrecognized)
// We skip all that and use simple packed C structs for learning clarity.
// ============================================================
#pragma pack(push, 1)

// ============================================================
// MESSAGE HEADER — prepended to every message on the wire
//
// WHY NEEDED: TCP is a byte stream — there are no message boundaries.
// If eNB sends 200 bytes and then 150 bytes, the receiver might get
// all 350 bytes in one recv() call, or split as 100+250, or any other way.
// ============================================================
struct MessageHeader {
    // Domain identifies the protocol family (4G EPC, 5G Core, IMS)
    // 0x01 = 4G, 0x02 = 5G, 0x03 = IMS
    uint8_t  domain;
    
    // Version allows protocol evolution without breaking the simulator
    uint8_t  version;

    // MessageType identifies the specific message (e.g., Attach Request)
    uint16_t msg_type;
    
    // Total bytes including this header
    uint32_t msg_length;    // total bytes of the message struct (including this header)
    
    // Monotonically increasing for correlation
    uint32_t sequence_num;  // monotonically increasing per sender, for correlation/debug

    // INTERVIEW: Operator Overloading
    // This allows us to log the header easily: std::cout << header;
    friend std::ostream& operator<<(std::ostream& os, const MessageHeader& hdr) {
        os << "Header[Domain: " << (int)hdr.domain 
           << ", Type: " << hdr.msg_type 
           << ", Len: " << hdr.msg_length << "] ";
        
        // ENGINEERING: Byte Pattern Visualization
        // Shows the raw hex representation of the header for debugging.
        char hex[32];
        snprintf(hex, sizeof(hex), " (Hex: %02X %02X %04X)", 
                 static_cast<int>(hdr.domain), static_cast<int>(hdr.version), 
                 static_cast<int>(hdr.msg_type));
        os << hex;
        return os;
    }
};

// INTERVIEW: Transport Layer Strategy (SCTP vs TCP)
// Senior Question: "S1AP requires SCTP. How does your simulator handle this on macOS?"
// Answer: "Since macOS lacks a native SCTP kernel stack, I've implemented a Transport
// Abstraction Layer. The code uses TCP with a 4-byte length-prefix (Message Shim) 
// to preserve SCTP's record-oriented nature. Crucially, the PcapWriter is 
// instrumented to wrap these payloads in valid SCTP headers for Wireshark, 
// ensuring 'Industry Standard' PCAP analysis (TS 36.413 compatibility)."

// INTERVIEW: Polymorphism
// Base class for all messages to demonstrate Virtual Functions
struct BaseMessage {
    virtual ~BaseMessage() = default; // Essential for correct cleanup in polymorphism
    virtual void logInterviewContext() const = 0; // Pure virtual for learning logic
};

// ============================================================
// NAS ATTACH REQUEST — TS 24.301 §8.2.4
//
// NAS = Non-Access Stratum. Layer 3 protocol between UE and MME.
// Travels inside S1AP NAS-PDU IE as opaque bytes.
// The eNB NEVER parses NAS — it treats it as a black box and tunnels it.
//
// LAYERING:
//   UE ←——NAS (EMM/ESM)——→ MME         (Layer 3 signalling)
//   UE ←——RRC——→ eNB                    (Radio Resource Control)
//   eNB ←——S1AP——→ MME                  (S1 control plane)
//   eNB wraps NAS in S1AP NAS-PDU IE and sends to MME.
//
// REAL NAS ENCODING: TLV (Type-Length-Value) per TS 24.007.
// Each IE: 1-2 byte IEI + 0-2 byte length + value bytes.
// Mandatory IEs in specific positions (no IEI/length needed).
// Here: simple struct encoding for learning.
//
// SECURITY: In 4G, Attach Request is sent PLAINTEXT (security_header_type=0x00)
// because no security context exists yet. This means:
//   - IMSI is in cleartext → IMSI catchers (IMSI grabbers/stingrays) exploit this
//   - Any radio sniffer can capture the IMSI
// 5G fixed this with SUCI: IMSI encrypted with home network's public key (ECIES)
// so only HSS/AUSF can decrypt it. eNB and other networks never see raw IMSI.
// ============================================================
struct NAS_AttachRequest {
    // Protocol discriminator — TS 24.007 §11.2.3.1.1
    // Identifies which NAS layer this message belongs to.
    //   0x02 = Call Control (CC)
    //   0x03 = SMS
    //   0x06 = Radio Resource Management
    //   0x07 = EPS Mobility Management (EMM) ← our case
    //   0x0A = EPS Session Management (ESM)
    uint8_t  protocol_discriminator;   // = 0x07

    // Security header type — TS 24.301 §9.3.1
    // 0x00 = plain NAS message (no integrity, no ciphering)
    // 0x01 = integrity protected (after SMC, SQN checked by MME)
    // 0x02 = integrity protected + ciphered
    // 0x04 = security mode command (special — partially protected)
    uint8_t  security_header_type;     // = 0x00 (cleartext on first attach)

    // Message type — TS 24.301 §9.8, Table 9.8.1
    // Identifies the specific EMM message within this discriminator.
    // 0x41 = Attach Request
    // 0x42 = Attach Accept      (MME → UE, Phase 2)
    // 0x44 = Attach Complete    (UE → MME, Phase 2)
    // 0x48 = Authentication Request (Phase 2)
    // 0x49 = Authentication Response (Phase 2)
    uint8_t  message_type;             // = 0x41

    // IE: EPS Attach Type — Mandatory, TS 24.301 §9.9.3.11
    // Tells MME what kind of attach the UE wants.
    // 1 = EPS attach         (LTE data only, no CS voice)
    // 2 = combined EPS/IMSI  (LTE data + CS voice via SGs to MSC)
    //     → used for CSFB: when UE gets a voice call, MME tells eNB
    //       to redirect UE to 2G/3G for the call, then back to LTE
    // INTERVIEW: "Combined attach = CSFB support. UE registers with
    //   both EPS domain (MME) and CS domain (MSC) simultaneously.
    //   MSC uses SGs interface to MME to page the UE for voice calls."
    uint8_t  eps_attach_type;          // = 1 (EPS-only, data)

    // IE: NAS Key Set Identifier (eKSI) — Mandatory, TS 24.301 §9.9.3.21
    // Points to which cached security context (key set) UE is using.
    // 0-6 = valid KSI (UE has a previous security context cached)
    //       MME can look up the keys by KSI and skip AKA authentication
    // 7   = 0b111 = "no key available" — UE has no cached context
    //       MME MUST run full AKA (Authentication and Key Agreement)
    // INTERVIEW: "This is how fast re-attach works. If UE was recently
    //   registered and still has the NAS security context, it sends KSI
    //   and MME can skip authentication. KSI=7 always means full AKA."
    uint8_t  nas_ksi;                  // = 7 (no key, first attach)

    // IE: EPS Mobile Identity — Mandatory, TS 24.301 §9.9.3.12
    // Identifies the UE. First attach: IMSI. Subsequent: GUTI.
    // GUTI = Globally Unique Temporary ID. Assigned by MME after auth.
    // Structure: <MCC><MNC><MMEGI><MMEC><M-TMSI>
    //   MCC+MNC identifies the PLMN (home network)
    //   MMEGI = MME Group ID (within PLMN)
    //   MMEC  = MME Code (within MME Group)
    //   M-TMSI = MME-assigned temporary ID (changes on each attach)
    // INTERVIEW: "GUTI hides the IMSI after first attach. UE uses GUTI
    //   for subsequent attaches. If MME can't find the GUTI (e.g., load
    //   balanced to a different MME), it requests the IMSI — which then
    //   goes cleartext again. 5G SUCI prevents this with encryption."
    uint8_t  identity_type;            // 1=IMSI, 6=GUTI
    uint64_t imsi;                     // 15-digit IMSI as uint64_t

    // IE: UE Network Capability — Mandatory, TS 24.301 §9.9.3.34
    // Bitmask of security algorithms the UE supports.
    // MME picks the "best" algorithm that both UE and MME support.
    // EEA = EPS Encryption Algorithm:
    //   EEA0 = null cipher (no encryption — only allowed for emergency calls)
    //   EEA1 = SNOW 3G (stream cipher, same as UMTS UEA2)
    //   EEA2 = AES-CTR  (AES in counter mode — most common in LTE)
    //   EEA3 = ZUC      (Chinese national algorithm, used by some operators)
    // EIA = EPS Integrity Algorithm:
    //   EIA0 = null integrity (FORBIDDEN except emergency — allows message injection!)
    //   EIA1 = SNOW 3G HMAC
    //   EIA2 = AES-CMAC  (most common)
    // INTERVIEW: "Why is EIA0 only for emergency? Integrity protects against
    //   man-in-the-middle modification of NAS messages. Without EIA, network
    //   could send fake Attach Reject or fake PDN Connectivity requests."
    uint8_t  ue_network_capability;    // = 0xE0 = 11100000b (EEA0/1/2 + EIA1/2)
};

// ============================================================
// S1AP INITIAL UE MESSAGE — TS 36.413 §9.1.7.1
//
// Sent by eNB when a UE with NO existing S1-AP context sends a NAS message.
// Triggers creation of a new UE context at the MME.
//
// S1-AP CONTEXT STATES:
//   Before this msg: UE is "S1-AP unassociated" — MME has no UE context
//   After this msg: MME creates context, assigns MME-UE-S1AP-ID
//   Future msgs:   Use (ENB-UE-S1AP-ID, MME-UE-S1AP-ID) pair to identify UE
//
// REAL ENCODING: ASN.1 SEQUENCE, APER encoded.
// Every IE is wrapped in a criticality-tagged container. The receiver
// must handle unknown IEs based on their criticality:
//   reject  → reject the entire message if IE unknown
//   ignore  → skip the unknown IE and continue processing
//   notify  → skip but send an error notification
//
// Our struct: flat binary layout for learning simplicity.
// ============================================================
struct S1AP_InitialUEMessage {
    // ---- Our framing header (not in real S1AP PDU format) ----
    MessageHeader header;

    // IE: eNB-UE-S1AP-ID — Mandatory [id=8, criticality=reject]
    // TS 36.413 §9.2.1.4
    // eNB's local handle for this UE. Range: 0..16777215 (24-bit).
    // Together with MME-UE-S1AP-ID, uniquely identifies a UE on S1.
    // Think of it as a "file descriptor" — eNB's internal reference.
    // MME must store this and use it in all messages it sends TO the eNB
    // (e.g., Downlink NAS Transport, Initial Context Setup Request).
    // REAL: ASN.1 INTEGER (0..16777215), APER: 3 bytes (24-bit constrained int)
    uint32_t  enb_ue_s1ap_id;

    // IE: NAS-PDU — Mandatory [id=26, criticality=reject]
    // TS 36.413 §9.2.3.4
    // Contains the NAS message (Attach Request here) as opaque bytes.
    // The eNB NEVER parses or modifies this. It is a transparent container.
    // MME decodes it using TS 24.301 NAS protocol rules.
    // INTERVIEW: "NAS-PDU is transparent to eNB because NAS is end-to-end
    //   between UE and core network. eNB is purely a radio relay for NAS.
    //   This is the same reason eNB doesn't know the UE's IP address."
    // REAL: OCTET STRING (variable length). Our cap: 512 bytes.
    uint16_t  nas_pdu_length;
    uint8_t   nas_pdu[512];

    // IE: TAI (Tracking Area Identity) — Mandatory [id=67, criticality=reject]
    // TS 36.413 §9.2.3.7 / TS 23.003 §19.4
    // Identifies the tracking area where the UE currently is.
    // TAI = PLMN Identity (MCC+MNC) + TAC (Tracking Area Code, 16-bit)
    // MME uses TAI for:
    //   1. TAI List in Attach Accept: UE can roam within these TAs without re-registering
    //   2. SGW selection: pick an SGW that serves this TA
    //   3. Paging: when network wants to reach UE, page all cells in UE's last known TA
    // REAL: pLMNidentity OCTET STRING(SIZE(3)) — BCD packed:
    //   MCC=404, MNC=10 → bytes: 0x04, 0xF4, 0x01
    //   (MCC digits: 4,0,4 | MNC digits: 1,0,F=filler for 2-digit MNC)
    uint16_t  tai_mcc;           // 404 (India)
    uint16_t  tai_mnc;           // 10  (Airtel/some operator)
    uint16_t  tai_tac;           // Tracking Area Code — operator-assigned

    // IE: E-UTRAN CGI (Cell Global Identity) — Mandatory [id=100, criticality=ignore]
    // TS 36.413 §9.2.1.38 / TS 23.003 §19.6
    // Exact cell identity: PLMN + 28-bit E-UTRAN Cell Identifier (eCI)
    // More specific than TAI — identifies the exact cell, not just the area.
    // Used for:
    //   1. Location services (LCS): report UE location to external clients
    //   2. Lawful intercept (LI): log which cell the UE is in
    //   3. SON (Self-Organizing Network): optimize network based on cell load
    //   4. Handover decisions in Phase 2+
    // REAL: PLMN(3 bytes BCD) + eUTRANcellIdentifier BIT STRING(SIZE(28))
    uint32_t  eutran_cgi_cell_id;    // 28-bit cell ID (top 4 bits unused)

    // IE: RRC Establishment Cause — Mandatory [id=134, criticality=ignore]
    // TS 36.413 §9.2.1.3a / TS 36.331 §6.2.2
    // Why the UE set up the RRC connection. MME uses this for:
    //   1. Admission control: reject low-priority if congested
    //   2. Overload control: reject mo-Data but still process mo-Signalling
    //   3. Priority handling: emergency always admitted
    // Values:
    //   0 = emergency           (highest priority, always admit, EIA0 allowed)
    //   1 = highPriorityAccess  (police, military, first responders — MCPTT)
    //   2 = mt-Access           (mobile-terminated: network paged the UE)
    //   3 = mo-Signalling       (UE-initiated signalling: Attach, TAU, Service Request)
    //   4 = mo-Data             (UE-initiated data: app opened, push notification)
    //   5 = delay-TolerantAccess (IoT/M2M: low-power sensors, not time-critical)
    // INTERVIEW: "If MME sends an Overload Start message to eNB, eNB applies
    //   the reduction percentage to mo-Data connections first. mo-Signalling
    //   and emergency are never reduced. This prevents signalling storms."
    uint8_t   rrc_establishment_cause;  // = 3 (mo-Signalling)
};

#pragma pack(pop)

// ============================================================
// HELPERS — serialize / deserialize NAS
// ============================================================

// Pack NAS_AttachRequest bytes into the nas_pdu buffer of InitialUEMessage.
// Returns number of bytes written. 0 if buf too small.
inline uint16_t serialize_nas_attach(const NAS_AttachRequest& nas,
                                      uint8_t* buf, uint16_t buf_size) {
    if (buf_size < static_cast<uint16_t>(sizeof(NAS_AttachRequest))) return 0;
    std::memcpy(buf, &nas, sizeof(NAS_AttachRequest));
    return static_cast<uint16_t>(sizeof(NAS_AttachRequest));
}

// Unpack NAS_AttachRequest from nas_pdu bytes.
inline bool deserialize_nas_attach(const uint8_t* buf, uint16_t len,
                                    NAS_AttachRequest& out) {
    if (len < static_cast<uint16_t>(sizeof(NAS_AttachRequest))) return false;
    std::memcpy(&out, buf, sizeof(NAS_AttachRequest));
    return true;
}
