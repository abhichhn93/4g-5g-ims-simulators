#pragma once
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

// ============================================================
// FLYWEIGHT PATTERN — SubscriberProfile
//
// PROBLEM without Flyweight:
//   1000 UEs, each storing APN, QCI, bitrates, charging flags
//   = 1000 copies of identical data for "internet" subscribers
//   Memory waste, cache thrash, harder to update policy globally
//
// FLYWEIGHT SOLUTION:
//   Separate intrinsic state (shared, immutable) from extrinsic (per-UE)
//   Intrinsic: APN="internet", QCI=9, 100Mbps DL → ONE shared object
//   Extrinsic: IMSI, IP address, TEID, state → per-UE UeContext
//   1000 UEs share ONE SubscriberProfile object via shared_ptr
//   Memory: 1000 × sizeof(shared_ptr) = 8KB instead of 1000 × profile = ~100KB
//
// INTERVIEW Q: "How would you reduce memory in an MME with 100K UEs?"
// ANSWER: "Flyweight pattern. Share immutable subscriber policy data
//   across UEs with the same profile. 100K UEs with 10 distinct APNs →
//   10 profile objects, 100K shared_ptr references. Also: compact binary
//   structs, memory pools for UeContext allocation (avoid malloc overhead),
//   columnar storage for analytics queries over UE attributes."
//
// REAL SYSTEM: Samsung's MME kept subscriber policy in a separate
//   Policy Manager module. The policy was loaded from HSS and cached.
//   Multiple UEs on same APN shared the cached policy entry.
//   In 5G: policy is fetched from PCF via JSON/HTTP (SBI) and cached in AMF.
//
// CLOUD-NATIVE: Subscriber profiles stored in Redis/Cassandra.
//   All AMF pods read from shared cache. Cache invalidation via Pub/Sub
//   (Kafka topic "policy-updates"). Flyweight at process level becomes
//   shared cache at cluster level.
// ============================================================
struct SubscriberProfile {
    // Intrinsic state — immutable after creation, safe to share without locking
    const std::string apn;
    const uint8_t     qci;              // QoS Class Indicator (9=internet, 1=VoLTE, 4=gaming)
    const uint32_t    max_ul_bps;       // Max uplink bitrate (bits/sec)
    const uint32_t    max_dl_bps;       // Max downlink bitrate (bits/sec)
    const bool        online_charging;  // Gy (real-time balance check)
    const bool        offline_charging; // Gz (CDR generation)
    const std::string charging_rule;    // Gx rule name (from PCRF)

    SubscriberProfile(std::string apn, uint8_t qci, uint32_t ul, uint32_t dl,
                      bool online=false, bool offline=true,
                      std::string rule="permit_all")
        : apn(std::move(apn)), qci(qci), max_ul_bps(ul), max_dl_bps(dl),
          online_charging(online), offline_charging(offline),
          charging_rule(std::move(rule)) {}
};

// ============================================================
// PROFILE REGISTRY — Flyweight Factory
//
// Pre-loads standard profiles at startup.
// Returns the SAME shared object for identical profiles.
// Thread-safe via shared_mutex (many concurrent reads, rare writes).
//
// PATTERN NOTE: This is a combination of:
//   Flyweight (shared immutable object)
//   Factory   (creates and caches objects)
//   Singleton (one registry per process — in real systems: one per pod)
// ============================================================
class ProfileRegistry {
public:
    ProfileRegistry() {
        // Standard profiles — one object shared by all UEs on this APN
        add("internet", std::make_shared<SubscriberProfile>(
            "internet", 9, 50'000'000, 100'000'000, false, true, "internet_rule"));
        add("ims", std::make_shared<SubscriberProfile>(
            "ims", 1, 5'000'000, 5'000'000, false, false, "ims_rule"));
        add("mms", std::make_shared<SubscriberProfile>(
            "mms", 9, 1'000'000, 2'000'000, false, true, "mms_rule"));
        add("gaming", std::make_shared<SubscriberProfile>(
            "gaming", 4, 10'000'000, 20'000'000, false, true, "gaming_rule"));
    }

    void add(const std::string& key, std::shared_ptr<const SubscriberProfile> p) {
        std::unique_lock<std::shared_mutex> lk(mutex_);
        profiles_[key] = std::move(p);
    }

    std::shared_ptr<const SubscriberProfile> get(const std::string& apn) const {
        std::shared_lock<std::shared_mutex> lk(mutex_);
        auto it = profiles_.find(apn);
        if (it != profiles_.end()) return it->second;
        // Default to "internet" profile if APN not found
        return profiles_.at("internet");
    }

    // Global singleton — all nodes share one registry per process
    static ProfileRegistry& instance() {
        static ProfileRegistry reg;
        return reg;
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const SubscriberProfile>> profiles_;
};
