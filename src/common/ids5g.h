#pragma once
#include <cstdlib>
#include <string>
#include <sstream>
#include <iomanip>

// ============================================================
// 5G SUBSCRIBER IDENTITIES — PLMN 404/10 by default (same test PLMN
// the 4G simulator uses), so the SAME subscriber numbering scheme
// shows up on both projects — e.g. UE #1 is "imsi-404100000000001"
// on both the 4G and 5G sides.
//
// The PLMN is operator-configurable via env vars PLMN_MCC / PLMN_MNC
// (see k8s/plmn-configmap.yaml) -- a real operator doesn't recompile
// the AMF/UDM/gNB to change which network they belong to.
//
//   SUPI  = Subscription Permanent Identifier (TS 23.501 §5.9.2)
//           here: "imsi-<mcc><mnc><msin>" — the IMSI form of SUPI.
//   SUCI  = Subscription Concealed Identifier (TS 23.003 §28.7.2)
//           the encrypted-over-the-air form of the SUPI. We use
//           "protection scheme 0" = NULL SCHEME, a real 3GPP-
//           defined option (TS 33.501 Annex C.2) where the
//           scheme-output is just the MSIN in plaintext — used on
//           test networks / when no home-network public key is
//           provisioned. That's why a SUCI here is "readable".
// ============================================================
namespace ids5g {

inline std::string mcc() {
    const char* v = std::getenv("PLMN_MCC");
    return v ? v : "404";
}

inline std::string mnc() {
    const char* v = std::getenv("PLMN_MNC");
    return v ? v : "10";
}

// 3GPP TS 23.003 §28.7.2 PLMN-domain form, e.g. MCC=404/MNC=10 ->
// "mnc010.mcc404" (the MNC is always zero-padded to 3 digits in the
// domain name, even for 2-digit MNCs). Used to build the
// "*.3gppnetwork.org" SBI hostnames.
inline std::string plmnDomain() {
    std::string m = mnc();
    if (m.size() == 2) m = "0" + m;
    return "mnc" + m + ".mcc" + mcc();
}

inline std::string msin(int ueId) {
    std::ostringstream ss;
    ss << std::setw(10) << std::setfill('0') << ueId;
    return ss.str();
}

// suci-<supi type=0:IMSI>-<MCC>-<MNC>-<routing indicator>-<scheme=0:null>-<home net key id=0>-<MSIN>
inline std::string suci(int ueId) {
    return "suci-0-" + mcc() + "-" + mnc() + "-0000-0-0-" + msin(ueId);
}

inline std::string supi(int ueId) {
    return "imsi-" + mcc() + mnc() + msin(ueId);
}

// Pull the MSIN (scheme-output) back out of a null-scheme SUCI.
inline std::string msinFromSuci(const std::string& s) {
    auto p = s.rfind('-');
    return (p == std::string::npos) ? "" : s.substr(p + 1);
}

inline std::string supiFromSuci(const std::string& s) {
    return "imsi-" + mcc() + mnc() + msinFromSuci(s);
}

} // namespace ids5g
