#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ecl {

class UdpSender {
  public:
    UdpSender() = default;
    ~UdpSender();

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    bool open(const std::string& dest_ip, uint16_t dest_port);
    void close();
    bool send_packet(const uint8_t* data, size_t len);
    bool is_open() const { return fd_ >= 0; }

  private:
    int fd_ = -1;
    uint32_t dest_addr_be_ = 0;
    uint16_t dest_port_be_ = 0;
};

class UdpReceiver {
  public:
    UdpReceiver() = default;
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    bool bind(uint16_t port, int timeout_ms = 50);
    void close();
    // Returns bytes received (>0), 0 on timeout, -1 on error.
    int recv_packet(uint8_t* buf, size_t capacity);
    uint16_t bound_port() const { return bound_port_; }
    bool is_open() const { return fd_ >= 0; }

  private:
    int fd_ = -1;
    uint16_t bound_port_ = 0;
};

}  // namespace ecl
