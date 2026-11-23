#include <cstdio>
#include <cstdlib>
#define ECL_CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (0)
#include <cstdio>
#include <cstring>
#include <vector>

#include "ecl/net_impairment.hpp"
#include "ecl/packetizer.hpp"
#include "ecl/protocol.hpp"
#include "ecl/udp_transport.hpp"

using namespace ecl;

namespace {

struct CollectCtx {
    std::vector<std::vector<uint8_t>> packets;
};

bool collect_packet(void* ctx, const uint8_t* data, size_t len) {
    auto* c = static_cast<CollectCtx*>(ctx);
    c->packets.emplace_back(data, data + len);
    return true;
}

void test_zero_loss_exact() {
    Packetizer pkt({1, 1400});
    Depacketizer depkt;
    ECL_CHECK(depkt.init({131072, 4, 3}));

    std::vector<uint8_t> payload(18543);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((i * 73 + 19) & 0xFFu);
    }

    CodecOutput co{};
    co.mode = CodecMode::PASS_THROUGH;
    co.frame_id = 42;
    co.reference_frame_id = 42;
    co.is_keyframe = true;
    co.bytes = ConstByteSpan{payload.data(), payload.size()};

    FrameMetadata meta{};
    meta.width = 640;
    meta.height = 480;
    meta.has_attitude = true;
    meta.pitch_cdeg = -325;
    meta.roll_cdeg = 140;

    CollectCtx col;
    uint8_t wire_buf[kDefaultMtuBytes];
    const size_t n = pkt.packetize(co, 1234567u, meta, wire_buf, sizeof(wire_buf), collect_packet, &col);
    ECL_CHECK(n == 14);
    ECL_CHECK(col.packets.size() == 14);

    PacketizedFrame out{};
    int completed = 0;
    for (const auto& p : col.packets) {
        if (depkt.feed_packet(p.data(), p.size(), &out)) {
            completed++;
        }
    }
    ECL_CHECK(completed == 1);
    ECL_CHECK(out.frame_id == 42);
    ECL_CHECK(out.reference_frame_id == 42);
    ECL_CHECK(out.is_keyframe);
    ECL_CHECK(out.timestamp_us_low == 1234567u);
    ECL_CHECK(out.metadata.width == 640 && out.metadata.height == 480);
    ECL_CHECK(out.metadata.has_attitude && out.metadata.pitch_cdeg == -325 && out.metadata.roll_cdeg == 140);
    ECL_CHECK(out.payload.size == payload.size());
    ECL_CHECK(std::memcmp(out.payload.data, payload.data(), payload.size()) == 0);
}

void test_out_of_order_tail_first_and_duplicates() {
    Packetizer pkt({1, 600});
    Depacketizer depkt;
    ECL_CHECK(depkt.init({65536, 4, 3}));

    std::vector<uint8_t> payload(4100);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i & 0xFFu);

    CodecOutput co{};
    co.mode = CodecMode::PASS_THROUGH;
    co.frame_id = 7;
    co.reference_frame_id = 7;
    co.is_keyframe = true;
    co.bytes = ConstByteSpan{payload.data(), payload.size()};

    CollectCtx col;
    uint8_t wire_buf[600];
    const size_t n = pkt.packetize(co, 100u, {}, wire_buf, sizeof(wire_buf), collect_packet, &col);
    ECL_CHECK(n > 3);

    // Feed last chunk FIRST, then reverse order, with duplicates!
    PacketizedFrame out{};
    int completed = 0;
    for (size_t i = col.packets.size(); i > 0; --i) {
        const auto& p = col.packets[i - 1];
        if (depkt.feed_packet(p.data(), p.size(), &out)) completed++;
        if (depkt.feed_packet(p.data(), p.size(), &out)) completed++;  // duplicate
    }
    ECL_CHECK(completed == 1);
    ECL_CHECK(depkt.stats().packets_duplicate == col.packets.size());
    ECL_CHECK(out.payload.size == payload.size());
    ECL_CHECK(std::memcmp(out.payload.data, payload.data(), payload.size()) == 0);
}

void test_sequence_wrap_around() {
    Packetizer pkt({1, 1400});
    Depacketizer depkt;
    ECL_CHECK(depkt.init({65536, 4, 3}));

    uint8_t wire_buf[kDefaultMtuBytes];
    uint32_t ids[6] = {0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x00000000u, 0x00000001u, 0x00000002u};
    for (uint32_t fid : ids) {
        std::vector<uint8_t> payload(2000, static_cast<uint8_t>(fid & 0xFFu));
        CodecOutput co{};
        co.mode = CodecMode::PASS_THROUGH;
        co.frame_id = fid;
        co.reference_frame_id = fid;
        co.is_keyframe = true;
        co.bytes = ConstByteSpan{payload.data(), payload.size()};

        CollectCtx col;
        ECL_CHECK(pkt.packetize(co, fid * 10u, {}, wire_buf, sizeof(wire_buf), collect_packet, &col) == 2);
        PacketizedFrame out{};
        bool done = false;
        for (const auto& p : col.packets) {
            if (depkt.feed_packet(p.data(), p.size(), &out)) done = true;
        }
        ECL_CHECK(done);
        ECL_CHECK(out.frame_id == fid);
        ECL_CHECK(out.payload.size == payload.size());
        ECL_CHECK(std::memcmp(out.payload.data, payload.data(), payload.size()) == 0);
    }
    ECL_CHECK(depkt.stats().frames_completed == 6);
}

void test_malformed_and_crc_errors() {
    Depacketizer depkt;
    ECL_CHECK(depkt.init({65536, 4, 3}));

    PacketizedFrame out{};
    // Too short
    uint8_t tiny[10]{};
    ECL_CHECK(!depkt.feed_packet(tiny, sizeof(tiny), &out));
    // Bad magic
    uint8_t bad[64]{};
    ECL_CHECK(!depkt.feed_packet(bad, sizeof(bad), &out));

    // Build a valid packet then flip 1 bit in payload -> CRC mismatch
    PacketHeader hdr{};
    hdr.frame_id = 1;
    hdr.reference_frame_id = 1;
    hdr.chunk_id = 0;
    hdr.chunk_count = 1;
    hdr.payload_len = 16;
    uint8_t body[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint8_t wire[128];
    const size_t wlen = pack_packet(hdr, body, wire, sizeof(wire));
    ECL_CHECK(wlen == kPacketOverheadBytes + 16);

    wire[kPacketHeaderBytes + 3] ^= 0x01;  // corrupt 1 bit
    ECL_CHECK(!depkt.feed_packet(wire, wlen, &out));
    ECL_CHECK(depkt.stats().packets_crc_error == 1);

    // Truncate packet
    wire[kPacketHeaderBytes + 3] ^= 0x01;  // repair bit
    ECL_CHECK(!depkt.feed_packet(wire, wlen - 2, &out));
    ECL_CHECK(depkt.stats().packets_malformed >= 4);
}

void test_impairment_determinism_and_expiration() {
    ImpairmentConfig cfg{};
    cfg.seed = 0x12345678ULL;
    cfg.random_loss_rate = 0.08;
    cfg.burst_enter_prob = 0.02;
    cfg.burst_stay_prob = 0.50;
    cfg.reorder_prob = 0.15;
    cfg.duplicate_prob = 0.05;
    cfg.truncate_prob = 0.03;
    cfg.max_mtu_bytes = 1400;

    auto run_once = [&]() {
        NetworkImpairmentSimulator sim(cfg);
        Packetizer pkt({1, 1200});
        Depacketizer depkt;
        ECL_CHECK(depkt.init({65536, 4, 3}));
        uint8_t wire_buf[1200];
        uint64_t completed = 0;
        for (uint32_t f = 0; f < 40; ++f) {
            std::vector<uint8_t> data(4500, static_cast<uint8_t>(f));
            CodecOutput co{};
            co.mode = CodecMode::PASS_THROUGH;
            co.frame_id = f;
            co.reference_frame_id = f;
            co.is_keyframe = true;
            co.bytes = ConstByteSpan{data.data(), data.size()};
            CollectCtx col;
            pkt.packetize(co, f * 33333u, {}, wire_buf, sizeof(wire_buf), collect_packet, &col);
            for (const auto& p : col.packets) {
                sim.send(p.data(), p.size(), f * 33333ULL);
            }
            auto ready = sim.drain_ready(f * 33333ULL + 5000ULL);
            PacketizedFrame pf{};
            for (const auto& rp : ready) {
                if (depkt.feed_packet(rp.data(), rp.size(), &pf)) completed++;
            }
        }
        auto tail = sim.drain_all();
        PacketizedFrame pf{};
        for (const auto& rp : tail) {
            if (depkt.feed_packet(rp.data(), rp.size(), &pf)) completed++;
        }
        depkt.flush_incomplete();
        ECL_CHECK(completed > 0 && completed < 40);
        ECL_CHECK(depkt.stats().frames_expired > 0);
        return std::pair<uint64_t, uint64_t>{completed, sim.stats().packets_delivered};
    };

    auto r1 = run_once();
    auto r2 = run_once();
    ECL_CHECK(r1 == r2);  // 100% deterministic!
}

void test_udp_loopback() {
    UdpReceiver rx;
    ECL_CHECK(rx.bind(0, 100));
    UdpSender tx;
    ECL_CHECK(tx.open("127.0.0.1", rx.bound_port()));

    PacketHeader hdr{};
    hdr.frame_id = 99;
    hdr.reference_frame_id = 99;
    hdr.chunk_id = 0;
    hdr.chunk_count = 1;
    hdr.payload_len = 5;
    const uint8_t body[5] = {'H', 'E', 'L', 'L', 'O'};
    uint8_t wire[128];
    const size_t wlen = pack_packet(hdr, body, wire, sizeof(wire));
    ECL_CHECK(tx.send_packet(wire, wlen));

    uint8_t rxbuf[256];
    const int n = rx.recv_packet(rxbuf, sizeof(rxbuf));
    ECL_CHECK(n == static_cast<int>(wlen));

    PacketHeader out_hdr{};
    ConstByteSpan out_payload{};
    ECL_CHECK(unpack_packet(rxbuf, static_cast<size_t>(n), &out_hdr, &out_payload) == UnpackStatus::OK);
    ECL_CHECK(out_hdr.frame_id == 99);
    ECL_CHECK(out_payload.size == 5 && std::memcmp(out_payload.data, "HELLO", 5) == 0);
}

}  // namespace

int main() {
    test_zero_loss_exact();
    test_out_of_order_tail_first_and_duplicates();
    test_sequence_wrap_around();
    test_malformed_and_crc_errors();
    test_impairment_determinism_and_expiration();
    test_udp_loopback();
    std::printf("test_transport: ALL PASSED\n");
    return 0;
}
