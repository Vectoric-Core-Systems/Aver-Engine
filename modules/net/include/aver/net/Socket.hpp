#pragma once
// Non-blocking UDP and TCP sockets over the OS (ws2_32 on Windows, BSD sockets elsewhere). No third-party code.
// G6 NET1 (https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G6_NETWORK.md).
//
// Return codes of the byte-moving calls: >= 0 is a byte count (a UDP datagram can be empty); the negatives below say why nothing moved.
// A socket never binds a wildcard address unless the caller passes one: bind(Address::any4()) is the caller's explicit request.
#include "aver/net/Address.hpp"

namespace aver::net {

inline constexpr int kWouldBlock = -1;
inline constexpr int kSockError  = -2;
inline constexpr int kSockClosed = -3;   // TCP: the peer closed in order

using NativeSocket = i64;                // SOCKET or fd; -1 = none

// Waits until the socket is readable (or writable) or the timeout passes. A negative timeout waits forever.
bool waitSocket(NativeSocket s, bool forWrite, int timeoutMs);

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() { close(); }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& o) noexcept : s_(o.s_), family_(o.family_) { o.s_ = -1; o.family_ = 0; }
    UdpSocket& operator=(UdpSocket&& o) noexcept { if (this != &o) { close(); s_ = o.s_; family_ = o.family_; o.s_ = -1; o.family_ = 0; } return *this; }

    bool open(int family);                       // 4 or 6; non-blocking
    bool bind(const Address& a);                 // opens the right family when closed
    void close();
    bool isOpen() const { return s_ != -1; }
    NativeSocket native() const { return s_; }
    Address localAddress() const;
    bool setBroadcast(bool on);
    bool setBufferSizes(int recvBytes, int sendBytes);
    int  sendTo(const Address& to, const u8* data, usize len);
    int  recvFrom(Address& from, u8* buf, usize cap);   // kWouldBlock when empty
    bool wait(int timeoutMs) const { return waitSocket(s_, false, timeoutMs); }
private:
    NativeSocket s_ = -1;
    int family_ = 0;
};

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket() { close(); }
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& o) noexcept : s_(o.s_), connecting_(o.connecting_) { o.s_ = -1; }
    TcpSocket& operator=(TcpSocket&& o) noexcept { if (this != &o) { close(); s_ = o.s_; connecting_ = o.connecting_; o.s_ = -1; } return *this; }

    bool beginConnect(const Address& to, const Address* from = nullptr);   // non-blocking; poll connectStatus()
    // 0 still connecting, 1 connected, -1 failed or never started.
    int  connectStatus();
    void close();
    bool isOpen() const { return s_ != -1; }
    NativeSocket native() const { return s_; }
    Address localAddress() const;
    Address peerAddress() const;
    bool setNoDelay(bool on);
    int  send(const u8* data, usize len);              // bytes taken, kWouldBlock, kSockError
    int  recv(u8* buf, usize cap);                     // bytes, kWouldBlock, kSockClosed, kSockError
    bool wait(bool forWrite, int timeoutMs) const { return waitSocket(s_, forWrite, timeoutMs); }
private:
    friend class TcpListener;
    NativeSocket s_ = -1;
    bool connecting_ = false;
};

class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener() { close(); }
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    bool listen(const Address& a, int backlog = 16);
    bool accept(TcpSocket& out, Address& from);        // false when nothing is waiting
    void close();
    bool isOpen() const { return s_ != -1; }
    NativeSocket native() const { return s_; }
    Address localAddress() const;
    bool wait(int timeoutMs) const { return waitSocket(s_, false, timeoutMs); }
private:
    NativeSocket s_ = -1;
};

} // namespace aver::net
