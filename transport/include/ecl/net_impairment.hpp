#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ecl {

struct ImpairmentConfig {
    uint64_t seed = 0x5EED123456789ABCULL;
    double random_loss_rate = 0.0;      // [0, 1]
    double burst_enter_prob = 0.0;      // Gilbert-Elliott enter bad state prob
    double burst_stay_prob = 0.0;       // Gilbert-Elliott stay in bad state prob
    double reorder_prob = 0.0;          // probability of adding extra reorder delay
    double duplicate_prob = 0.0;        // probability of duplicating a packet
    double truncate_prob = 0.0;         // probability of truncating a packet
    size_t truncate_bytes = 8;          // bytes chopped off when truncated
    uint32_t base_delay_us = 0;         // base one-way delay
    uint32_t jitter_us = 0;             // uniform jitter [0, jitter_us]
    uint32_t reorder_extra_delay_us = 2500;
    size_t max_mtu_bytes = 1500;        // drop packets exceeding this MTU
};

struct ImpairedPacket {
    uint64_t deliver_time_us = 0;
    uint64_t sequence_order = 0;
    std::vector<uint8_t> bytes;
};

struct ImpairmentStats {
    uint64_t packets_in = 0;
    uint64_t packets_delivered = 0;
    uint64_t packets_dropped_random = 0;
    uint64_t packets_dropped_burst = 0;
    uint64_t packets_dropped_mtu = 0;
    uint64_t packets_duplicated = 0;
    uint64_t packets_reordered = 0;
    uint64_t packets_truncated = 0;
};

class NetworkImpairmentSimulator {
  public:
    explicit NetworkImpairmentSimulator(ImpairmentConfig cfg = {})
        : cfg_(cfg), rng_state_(cfg.seed ? cfg.seed : 0x123456789ABCDEF0ULL) {}

    void reset(ImpairmentConfig cfg) {
        cfg_ = cfg;
        rng_state_ = cfg.seed ? cfg.seed : 0x123456789ABCDEF0ULL;
        in_burst_ = false;
        seq_counter_ = 0;
        queue_.clear();
        stats_ = {};
    }

    // Submits one outgoing wire packet at `send_time_us`.
    void send(const uint8_t* data, size_t len, uint64_t send_time_us);

    // Drains all packets whose deliver_time_us <= `now_us` in delivery order.
    std::vector<std::vector<uint8_t>> drain_ready(uint64_t now_us);

    // Drains all remaining queued packets in delivery order.
    std::vector<std::vector<uint8_t>> drain_all();

    const ImpairmentStats& stats() const { return stats_; }

  private:
    uint64_t next_u64();
    double next_unit();

    ImpairmentConfig cfg_{};
    uint64_t rng_state_ = 0;
    bool in_burst_ = false;
    uint64_t seq_counter_ = 0;
    std::vector<ImpairedPacket> queue_;
    ImpairmentStats stats_{};
};

}  // namespace ecl
