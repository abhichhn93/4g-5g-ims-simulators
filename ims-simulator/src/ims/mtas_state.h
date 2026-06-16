#pragma once
// ============================================================
// MTAS shared state — call barring toggle via server stdin
//
// Real IMS: MTAS reads barring flags from HSS subscriber profile
// (downloaded at REGISTER time via Cx SAA / Sp interface).
// Simulation: server operator types BARR A|B|C / UNBARR A|B|C.
// ============================================================
#include <mutex>
#include <set>
#include <string>

namespace MtasState {
    inline std::mutex& mtx() {
        static std::mutex m;
        return m;
    }
    inline std::set<std::string>& barred() {
        static std::set<std::string> s;
        return s;
    }
    inline bool isBarred(const std::string& impu) {
        std::lock_guard<std::mutex> lk(mtx());
        return barred().count(impu) > 0;
    }
    inline void setBarred(const std::string& impu, bool b) {
        std::lock_guard<std::mutex> lk(mtx());
        if (b) barred().insert(impu);
        else   barred().erase(impu);
    }
}
