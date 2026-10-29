#pragma once
// The datagram seam under the reliable-UDP host: an OS UDP socket, or SimNetwork, an in-memory network on a virtual clock that loses,
// delays, duplicates and reorders datagrams from a seed so the protocol is tested without real sockets.
#include "aver/net/Address.hpp"
#include "aver/net/Socket.hpp"

#include <map>
#include <memory>
#include <queue>
#include <set>
#include <vector>

namespace aver::net {

class DatagramSocket {
public:
    virtual ~DatagramSocket() = default;
    virtual int  send(const Address& to, const u8* data, usize len) = 0;     // bytes, or a negative error
    virtual int  recv(Address& from, u8* buf, usize cap) = 0;                // bytes, kWouldBlock when empty
    virtual Address localAddress() const = 0;
    // Blocks until a datagram may be readable. The simulated socket returns at once.
    virtual bool wait(int timeoutMs) { (void)timeoutMs; return true; }
    virtual NativeSocket native() const { return -1; }
};

class UdpDatagramSocket final : public DatagramSocket {
public:
    explicit UdpDatagramSocket(UdpSocket&& s) : s_(std::move(s)) {}
    int send(const Address& to, const u8* data, usize len) override { return s_.sendTo(to, data, len); }
    int recv(Address& from, u8* buf, usize cap) override { return s_.recvFrom(from, buf, cap); }
    Address localAddress() const override { return s_.localAddress(); }
    bool wait(int timeoutMs) override { return s_.wait(timeoutMs); }
    NativeSocket native() const override { return s_.native(); }
    UdpSocket& socket() { return s_; }
private:
    UdpSocket s_;
};

// What the simulated wire does to a datagram. Probabilities are 0..1; delays are milliseconds of virtual time.
struct LinkConfig {
    double lossProbability = 0;
    double duplicateProbability = 0;
    u32 minDelayMs = 0;
    u32 maxDelayMs = 0;           // each datagram's delay is uniform in [min, max]: a wide range reorders
    usize maxDatagram = 65507;    // larger datagrams are dropped, like a path MTU that does not fragment
};

class SimSocket;

class SimNetwork {
public:
    explicit SimNetwork(u64 seed = 1) : rng_(seed ? seed : 1) {}
    ~SimNetwork();
    SimNetwork(const SimNetwork&) = delete;
    SimNetwork& operator=(const SimNetwork&) = delete;

    u64 now() const { return now_; }
    void advance(u64 ms) { now_ += ms; }
    void setNow(u64 ms) { now_ = ms; }

    LinkConfig config;                                   // every link
    void setLink(const Address& from, const Address& to, const LinkConfig& c) { links_[{from, to}] = c; }
    void blackhole(const Address& a, bool on) { if (on) dead_.insert(a); else dead_.erase(a); }   // drops all traffic to and from `a`

    // A socket at `at` (port 0 picks a free one). Null when the address is taken.
    std::unique_ptr<DatagramSocket> bind(Address at);

    struct Stats { u64 sent = 0, dropped = 0, duplicated = 0, delivered = 0; } stats;

private:
    friend class SimSocket;
    struct InFlight {
        u64 at; u64 seq; Address from, to; std::vector<u8> bytes;
        bool operator>(const InFlight& o) const { return at != o.at ? at > o.at : seq > o.seq; }
    };
    u64 rnd();
    double unit() { return static_cast<double>(rnd() >> 11) * (1.0 / 9007199254740992.0); }
    void post(const Address& from, const Address& to, const u8* data, usize len);
    void deliverDue();

    u64 rng_;
    u64 now_ = 0;
    u64 seq_ = 0;
    u16 nextPort_ = 40000;
    std::map<Address, SimSocket*> sockets_;
    std::map<std::pair<Address, Address>, LinkConfig> links_;
    std::set<Address> dead_;
    std::priority_queue<InFlight, std::vector<InFlight>, std::greater<InFlight>> wire_;
};

class SimSocket final : public DatagramSocket {
public:
    ~SimSocket() override;
    int send(const Address& to, const u8* data, usize len) override;
    int recv(Address& from, u8* buf, usize cap) override;
    Address localAddress() const override { return addr_; }
private:
    friend class SimNetwork;
    struct Msg { Address from; std::vector<u8> bytes; };
    SimNetwork* net_ = nullptr;
    Address addr_;
    std::queue<Msg> inbox_;
};

} // namespace aver::net
