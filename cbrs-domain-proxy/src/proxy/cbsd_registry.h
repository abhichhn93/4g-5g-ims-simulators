#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <functional>
#include "proxy/spectrum_state.h"

// ============================================================
// CBSD REGISTRY — in-memory store of all CBSDs known to this
// Domain Proxy.
//
// A real Domain Proxy would persist this to a database and
// survive restarts. Here we keep it in memory for simplicity.
//
// C++ design note: The registry is shared between the main
// thread (accept loop) and per-CBSD worker threads, so all
// access is protected by a std::mutex.  std::unordered_map
// gives O(1) average lookup by cbsdId — the typical operation.
// ============================================================

struct CbsdInfo {
    std::string cbsdId;        // Assigned by SAS on successful registration
    std::string fccId;         // FCC-issued equipment authorization ID
    std::string callSign;      // Operator call sign (e.g. "KA1ABC")
    std::string category;      // "A" (indoor, low power) or "B" (outdoor)
    double      latitude{};    // Installation location
    double      longitude{};
    double      heightMeters{};
    double      antennaGainDbi{};
    SpectrumState state{ SpectrumState::UNREGISTERED };

    // Grant fields — populated after GrantResponse from SAS
    std::string grantId;
    double      grantFreqMHz{};
    double      grantBwMHz{};
    double      maxEirp{};
};

class CbsdRegistry {
public:
    void upsert(const CbsdInfo& info) {
        std::lock_guard<std::mutex> lk(mu_);
        store_[info.cbsdId] = info;
    }

    bool get(const std::string& cbsdId, CbsdInfo& out) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = store_.find(cbsdId);
        if (it == store_.end()) return false;
        out = it->second;
        return true;
    }

    void setState(const std::string& cbsdId, SpectrumState s) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = store_.find(cbsdId);
        if (it != store_.end()) it->second.state = s;
    }

    void setGrant(const std::string& cbsdId, const std::string& grantId,
                  double freqMHz, double bwMHz, double maxEirp) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = store_.find(cbsdId);
        if (it == store_.end()) return;
        it->second.grantId      = grantId;
        it->second.grantFreqMHz = freqMHz;
        it->second.grantBwMHz   = bwMHz;
        it->second.maxEirp      = maxEirp;
        it->second.state        = SpectrumState::GRANTED;
    }

    void clearGrant(const std::string& cbsdId) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = store_.find(cbsdId);
        if (it == store_.end()) return;
        it->second.grantId = "";
        it->second.state   = SpectrumState::REGISTERED;
    }

    void remove(const std::string& cbsdId) {
        std::lock_guard<std::mutex> lk(mu_);
        store_.erase(cbsdId);
    }

    size_t count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return store_.size();
    }

    // Iterate over a snapshot — callback is called WITHOUT the lock held
    void forEach(std::function<void(const CbsdInfo&)> cb) const {
        std::vector<CbsdInfo> snapshot;
        { std::lock_guard<std::mutex> lk(mu_);
          for (auto& [id, info] : store_) snapshot.push_back(info); }
        for (auto& info : snapshot) cb(info);
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, CbsdInfo> store_;
};
