#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include "mme/ue_context.h"

// ============================================================
// SHARDED UE CONTEXT STORE
//
// PROBLEM: A single global mutex for the UE map serializes all threads:
//   - Thread A (eNB receive): holds lock while looking up a UE
//   - Thread B (STATUS):      blocked waiting for the lock
//   - Thread C (GTP-C recv):  also blocked
//   As UE count grows, lock contention kills throughput.
//
// SOLUTION: Shard the map into N buckets, each with its OWN mutex.
//   Thread A locks bucket 7  → Thread B and C lock bucket 23 → no conflict!
//   Only threads targeting the SAME bucket contend.
//   With 64 buckets and uniform hash: 64x lower lock contention.
//
// IMPLEMENTATION: hash(mme_ue_s1ap_id) % 64 → selects the bucket
//
// LOCK TYPE: std::shared_mutex (C++17) — reader-writer lock
//   - shared_lock:  multiple threads can READ simultaneously (STATUS)
//   - unique_lock:  only ONE thread can WRITE (attach/detach)
//   Best for: many reads, few writes — typical in an MME
//
// INTERVIEW Q: "How do you reduce lock contention in a high-throughput UE store?"
// ANSWER: "Shard the hash map — divide into N buckets, each with its own
//   mutex. Threads on different UEs hit different buckets = no contention.
//   Use shared_mutex for reader-writer semantics since reads (lookups)
//   are much more frequent than writes (attach/detach).
//   Samsung's MME used 100 shards keyed by last 2 IMSI digits."
//
// PHASE 4 EVOLUTION:
//   - 64 buckets → 256 buckets (more threads = more shards needed)
//   - shared_mutex → std::atomic operations for read-heavy hot paths
//   - In-process store → Redis/etcd (for 5G stateless AMF across pods)
//   - In K8s: each pod has its own shard range, consistent hashing routes requests
// ============================================================
class UeContextStore {
public:
    static constexpr size_t NUM_SHARDS = 64;

    struct Shard {
        mutable std::shared_mutex mutex;  // reader-writer lock
        std::unordered_map<uint32_t, std::shared_ptr<UeContext>> map;
    };

    // Insert a new UE context. Assigns mme_ue_s1ap_id atomically.
    // Returns the assigned ID.
    uint32_t insert(std::shared_ptr<UeContext> ctx) {
        uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
        ctx->mme_ue_s1ap_id = id;
        auto& sh = shard(id);
        std::unique_lock<std::shared_mutex> lk(sh.mutex);  // exclusive write
        sh.map[id] = std::move(ctx);
        return id;
    }

    // Look up by mme_ue_s1ap_id. Returns nullptr if not found.
    // Multiple threads can call find() simultaneously — shared_lock allows this.
    std::shared_ptr<UeContext> find(uint32_t id) const {
        const auto& sh = shard(id);
        std::shared_lock<std::shared_mutex> lk(sh.mutex);  // shared read
        auto it = sh.map.find(id);
        return it != sh.map.end() ? it->second : nullptr;
    }

    void remove(uint32_t id) {
        auto& sh = shard(id);
        std::unique_lock<std::shared_mutex> lk(sh.mutex);
        sh.map.erase(id);
    }

    size_t size() const {
        size_t total = 0;
        for (const auto& sh : shards_) {
            std::shared_lock<std::shared_mutex> lk(sh.mutex);
            total += sh.map.size();
        }
        return total;
    }

    // Iterate all UE contexts — locks each shard in turn
    void forEach(const std::function<void(const UeContext&)>& fn) const {
        for (const auto& sh : shards_) {
            std::shared_lock<std::shared_mutex> lk(sh.mutex);
            for (const auto& [id, ctx] : sh.map) fn(*ctx);
        }
    }

private:
    std::array<Shard, NUM_SHARDS> shards_;
    std::atomic<uint32_t>         next_id_{1};

    Shard& shard(uint32_t id) {
        // Hash: id % 64. Simple, uniform for sequential IDs.
        return shards_[id % NUM_SHARDS];
    }
    const Shard& shard(uint32_t id) const {
        return shards_[id % NUM_SHARDS];
    }
};
