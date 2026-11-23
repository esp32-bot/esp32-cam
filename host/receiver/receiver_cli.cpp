#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ecl/packetizer.hpp"
#include "ecl/temporal_codec.hpp"
#include "ecl/udp_transport.hpp"

namespace fs = std::filesystem;
using namespace ecl;

// Recording file format (deterministic offline receiver input):
// Repeated records of [uint32_le packet_len][packet_len bytes of ECL1 wire packet].
int main(int argc, char** argv) {
    uint16_t udp_port = 0;
    std::string recording_path;
    std::string out_dir;
    int max_frames = 300;
    int idle_timeouts = 20;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--udp-port" && i + 1 < argc) udp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--recording" && i + 1 < argc) recording_path = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (a == "--max-frames" && i + 1 < argc) max_frames = std::atoi(argv[++i]);
    }

    if (udp_port == 0 && recording_path.empty()) {
        std::fprintf(stderr, "Usage: ecl_receiver (--udp-port <port> | --recording <file>) [--out <dir>] [--max-frames N]\n");
        return 1;
    }
    if (!out_dir.empty()) fs::create_directories(out_dir);

    TemporalCodecConfig cfg{};
    cfg.max_frame_bytes = 262144;
    cfg.max_blocks = 16384;
    cfg.max_dependency_depth = 60;

    Depacketizer depkt;
    TemporalDecoderContext dec;
    if (!depkt.init({262144, 4, 3}) || !dec.init(cfg)) {
        std::fprintf(stderr, "Failed to init receiver\n");
        return 1;
    }

    int received_frames = 0;
    auto handle_wire = [&](const uint8_t* data, size_t len) {
        PacketizedFrame pf{};
        if (!depkt.feed_packet(data, len, &pf)) return;
        ReconstructedFrame rf{};
        if (codec_decode(&dec, pf, &rf) != DecodeStatus::OK) return;
        received_frames++;
        if (!out_dir.empty()) {
            char name[64];
            std::snprintf(name, sizeof(name), "rx_%06u.jpg", rf.frame_id);
            std::ofstream out(fs::path(out_dir) / name, std::ios::binary);
            out.write(reinterpret_cast<const char*>(rf.bytes.data), static_cast<std::streamsize>(rf.bytes.size));
        }
    };

    if (!recording_path.empty()) {
        std::ifstream in(recording_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "Failed to open recording %s\n", recording_path.c_str());
            return 1;
        }
        std::vector<uint8_t> buf(2048);
        while (in && received_frames < max_frames) {
            uint8_t len_le[4];
            if (!in.read(reinterpret_cast<char*>(len_le), 4)) break;
            const uint32_t plen = static_cast<uint32_t>(len_le[0]) |
                                  (static_cast<uint32_t>(len_le[1]) << 8) |
                                  (static_cast<uint32_t>(len_le[2]) << 16) |
                                  (static_cast<uint32_t>(len_le[3]) << 24);
            if (plen == 0 || plen > 65536) break;
            buf.resize(plen);
            if (!in.read(reinterpret_cast<char*>(buf.data()), plen)) break;
            handle_wire(buf.data(), buf.size());
        }
        depkt.flush_incomplete();
    } else {
        UdpReceiver rx;
        if (!rx.bind(udp_port, 100)) {
            std::fprintf(stderr, "Failed to bind UDP port %u\n", udp_port);
            return 1;
        }
        std::vector<uint8_t> buf(2048);
        int timeouts = 0;
        while (received_frames < max_frames && timeouts < idle_timeouts) {
            const int n = rx.recv_packet(buf.data(), buf.size());
            if (n > 0) {
                timeouts = 0;
                handle_wire(buf.data(), static_cast<size_t>(n));
            } else if (n == 0) {
                timeouts++;
            } else {
                break;
            }
        }
        depkt.flush_incomplete();
    }

    std::printf(
        "{\"frames_decoded\":%llu,\"packets_valid\":%llu,\"packets_malformed\":%llu,"
        "\"frames_expired\":%llu,\"missing_ref_drops\":%llu}\n",
        static_cast<unsigned long long>(dec.stats().frames_decoded),
        static_cast<unsigned long long>(depkt.stats().packets_valid),
        static_cast<unsigned long long>(depkt.stats().packets_malformed),
        static_cast<unsigned long long>(depkt.stats().frames_expired),
        static_cast<unsigned long long>(dec.stats().missing_reference_drops));
    return 0;
}
