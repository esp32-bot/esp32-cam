#include "ecl/net_impairment.hpp"

#include <algorithm>

namespace ecl {

uint64_t NetworkImpairmentSimulator::next_u64() {
    // SplitMix64: deterministic, fast, portable across platforms.
    uint64_t z = (rng_state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double NetworkImpairmentSimulator::next_unit() {
    return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

void NetworkImpairmentSimulator::send(const uint8_t* data, size_t len, uint64_t send_time_us) {
    stats_.packets_in++;
    if (!data || len == 0) return;

    if (cfg_.max_mtu_bytes > 0 && len > cfg_.max_mtu_bytes) {
        stats_.packets_dropped_mtu++;
        return;
    }

    // Gilbert-Elliott 2-state burst loss model.
    if (cfg_.burst_enter_prob > 0.0 || in_burst_) {
        if (in_burst_) {
            if (next_unit() >= cfg_.burst_stay_prob) {
                in_burst_ = false;
            }
        } else {
            if (next_unit() < cfg_.burst_enter_prob) {
                in_burst_ = true;
            }
        }
        if (in_burst_) {
            stats_.packets_dropped_burst++;
            return;
        }
    }

    if (cfg_.random_loss_rate > 0.0 && next_unit() < cfg_.random_loss_rate) {
        stats_.packets_dropped_random++;
        return;
    }

    auto make_packet = [&](uint64_t extra_delay_us) {
        ImpairedPacket pkt;
        const uint32_t jitter =
            cfg_.jitter_us > 0 ? static_cast<uint32_t>(next_u64() % (cfg_.jitter_us + 1u)) : 0u;
        pkt.deliver_time_us = send_time_us + cfg_.base_delay_us + jitter + extra_delay_us;
        pkt.sequence_order = seq_counter_++;
        size_t out_len = len;
        if (cfg_.truncate_prob > 0.0 && next_unit() < cfg_.truncate_prob) {
            out_len = (len > cfg_.truncate_bytes) ? (len - cfg_.truncate_bytes) : (len / 2);
            stats_.packets_truncated++;
        }
        pkt.bytes.assign(data, data + out_len);
        queue_.push_back(std::move(pkt));
    };

    uint64_t reorder_delay = 0;
    if (cfg_.reorder_prob > 0.0 && next_unit() < cfg_.reorder_prob) {
        reorder_delay = cfg_.reorder_extra_delay_us;
        stats_.packets_reordered++;
    }

    make_packet(reorder_delay);

    if (cfg_.duplicate_prob > 0.0 && next_unit() < cfg_.duplicate_prob) {
        stats_.packets_duplicated++;
        make_packet(reorder_delay + 150u);
    }
}

std::vector<std::vector<uint8_t>> NetworkImpairmentSimulator::drain_ready(uint64_t now_us) {
    std::stable_sort(queue_.begin(), queue_.end(), [](const ImpairedPacket& a, const ImpairedPacket& b) {
        if (a.deliver_time_us != b.deliver_time_us) {
            return a.deliver_time_us < b.deliver_time_us;
        }
        return a.sequence_order < b.sequence_order;
    });

    std::vector<std::vector<uint8_t>> ready;
    size_t consumed = 0;
    while (consumed < queue_.size() && queue_[consumed].deliver_time_us <= now_us) {
        ready.push_back(std::move(queue_[consumed].bytes));
        stats_.packets_delivered++;
        consumed++;
    }
    if (consumed > 0) {
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    return ready;
}

std::vector<std::vector<uint8_t>> NetworkImpairmentSimulator::drain_all() {
    return drain_ready(UINT64_MAX);
}

}  // namespace ecl
