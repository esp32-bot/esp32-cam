#include "ecl/udp_transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace ecl {

UdpSender::~UdpSender() { close(); }

bool UdpSender::open(const std::string& dest_ip, uint16_t dest_port) {
    close();
    in_addr addr{};
    if (::inet_pton(AF_INET, dest_ip.c_str(), &addr) != 1) {
        return false;
    }
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    dest_addr_be_ = addr.s_addr;
    dest_port_be_ = htons(dest_port);
    return true;
}

void UdpSender::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool UdpSender::send_packet(const uint8_t* data, size_t len) {
    if (fd_ < 0 || !data || len == 0) return false;
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = dest_addr_be_;
    sin.sin_port = dest_port_be_;
    const ssize_t sent = ::sendto(fd_, data, len, 0,
                                  reinterpret_cast<const sockaddr*>(&sin), sizeof(sin));
    return sent == static_cast<ssize_t>(len);
}

UdpReceiver::~UdpReceiver() { close(); }

bool UdpReceiver::bind(uint16_t port, int timeout_ms) {
    close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;

    int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (timeout_ms > 0) {
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(port);
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin)) < 0) {
        close();
        return false;
    }

    socklen_t slen = sizeof(sin);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&sin), &slen) == 0) {
        bound_port_ = ntohs(sin.sin_port);
    } else {
        bound_port_ = port;
    }
    return true;
}

void UdpReceiver::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    bound_port_ = 0;
}

int UdpReceiver::recv_packet(uint8_t* buf, size_t capacity) {
    if (fd_ < 0 || !buf || capacity == 0) return -1;
    const ssize_t n = ::recvfrom(fd_, buf, capacity, 0, nullptr, nullptr);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    return static_cast<int>(n);
}

}  // namespace ecl
