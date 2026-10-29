// The C seam onto modules/net (net_abi.h).

#include "aver/net/net_abi.h"

#include "aver/net/Protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>

using namespace aver;
using namespace aver::net;

static_assert(static_cast<int>(EventType::Connect) == AVER_NET_EVENT_CONNECT && static_cast<int>(EventType::Disconnect) == AVER_NET_EVENT_DISCONNECT &&
              static_cast<int>(EventType::Receive) == AVER_NET_EVENT_RECEIVE, "net_abi.h mirrors the event types");
static_assert(static_cast<int>(DisconnectReason::ConnectFailed) == AVER_NET_REASON_CONNECT_FAILED && static_cast<int>(DisconnectReason::Timeout) == AVER_NET_REASON_TIMEOUT,
              "net_abi.h mirrors the disconnect reasons");
static_assert(static_cast<int>(PeerState::Connected) == AVER_NET_PEER_STATE_CONNECTED && static_cast<int>(PeerState::Zombie) == AVER_NET_PEER_STATE_ZOMBIE &&
              static_cast<int>(PeerState::DisconnectLater) == AVER_NET_PEER_STATE_DISCONNECT_LATER, "net_abi.h mirrors the peer states");
static_assert(kFlagReliable == AVER_NET_SEND_FLAG_RELIABLE && kFlagUnsequenced == AVER_NET_SEND_FLAG_UNSEQUENCED, "net_abi.h mirrors the send flags");

namespace {

enum class Kind { Udp, Tcp, Listener };

struct SockEntry {
    Kind kind = Kind::Udp;
    int family = 4;
    UdpSocket udp;
    TcpSocket tcp;
    TcpListener listener;
};

struct HostEntry {
    std::unique_ptr<Host> host;
    Event last;
};

std::map<i32, std::unique_ptr<SockEntry>> g_socks;
std::map<i32, std::unique_ptr<HostEntry>> g_hosts;
i32 g_nextSock = 1, g_nextHost = 1;

i64 clockMs() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() + 1;
}

SockEntry* sock(i32 h, Kind k) {
    const auto it = g_socks.find(h);
    return it != g_socks.end() && it->second->kind == k ? it->second.get() : nullptr;
}
HostEntry* hostOf(i32 h) {
    const auto it = g_hosts.find(h);
    return it != g_hosts.end() ? it->second.get() : nullptr;
}
Peer* peerOf(i32 host, i64 key) {
    HostEntry* h = hostOf(host);
    if (!h || key <= 0) return nullptr;
    return h->host->peerByKey(static_cast<u16>(key & 0xFFFF), static_cast<u32>(key >> 16));
}
i64 keyOf(const Peer& p) { return (static_cast<i64>(p.generation()) << 16) | p.index(); }

bool parseAddr(const char* ip, i32 port, Address& out) {
    if (!ip || port < 0 || port > 65535) return false;
    return Address::parse(ip, static_cast<u16>(port), out);
}

// "*" is the wildcard of `family`; anything else is numeric text, or a name the system resolver knows.
bool bindAddr(const char* ip, i32 port, int family, Address& out) {
    if (!ip || port < 0 || port > 65535) return false;
    if (std::strcmp(ip, "*") == 0) { out = family == 6 ? Address::any6(static_cast<u16>(port)) : Address::any4(static_cast<u16>(port)); return true; }
    if (Address::parse(ip, static_cast<u16>(port), out)) return true;
    std::vector<Address> v;
    if (!resolve(ip, family, v)) return false;
    out = v.front();
    out.port = static_cast<u16>(port);
    return true;
}

i32 putText(const std::string& s, uint8_t* out, i32 cap) {
    const i32 n = static_cast<i32>(s.size());
    if (!out || cap <= n) return -(n + 1);
    std::memcpy(out, s.data(), static_cast<usize>(n));
    out[n] = 0;
    return n;
}

void putAddr(const Address& a, uint8_t* ipOut, i32 ipCap, i32* portOut) {
    if (ipOut && ipCap > 0) {
        const std::string s = a.ipString();
        const i32 n = std::min<i32>(static_cast<i32>(s.size()), ipCap - 1);
        std::memcpy(ipOut, s.data(), static_cast<usize>(n));
        ipOut[n] = 0;
    }
    if (portOut) *portOut = a.port;
}

i32 addSock(std::unique_ptr<SockEntry> e) {
    const i32 id = g_nextSock++;
    g_socks[id] = std::move(e);
    return id;
}

std::string joinAddrs(const std::vector<Address>& v) {
    std::string s;
    for (const Address& a : v) { if (!s.empty()) s += '\n'; s += a.ipString(); }
    return s;
}

} // namespace

extern "C" {

AVER_NET_API int32_t aver_net_abi_version(void) { return AVER_NET_ABI_VERSION; }
AVER_NET_API int64_t aver_net_now_ms(void) { return clockMs(); }

AVER_NET_API int32_t aver_net_resolve(const char* hostName, int32_t ipType, uint8_t* out, int32_t capacity) {
    std::vector<Address> v;
    if (!hostName || (ipType != 0 && ipType != 4 && ipType != 6) || !resolve(hostName, ipType, v)) return 0;
    return putText(joinAddrs(v), out, capacity);
}

AVER_NET_API int32_t aver_net_local_addresses(uint8_t* out, int32_t capacity) {
    std::string s;
    for (const std::string& a : localAddresses()) { if (!s.empty()) s += '\n'; s += a; }
    return putText(s, out, capacity);
}

// ---- UDP ----

AVER_NET_API int32_t aver_net_udp_open(int32_t family) {
    auto e = std::make_unique<SockEntry>();
    e->kind = Kind::Udp;
    if (!e->udp.open(family)) return 0;
    e->family = family;
    return addSock(std::move(e));
}

AVER_NET_API int32_t aver_net_udp_bind(int32_t s, const char* ip, int32_t port) {
    SockEntry* e = sock(s, Kind::Udp);
    if (!e) return 0;
    Address a;
    if (!bindAddr(ip, port, e->family, a)) return 0;
    if (a.family != e->family) {                              // an address of the other family: reopen in it
        if (!e->udp.open(a.family)) return 0;
        e->family = a.family;
    }
    return e->udp.bind(a) ? 1 : 0;
}

AVER_NET_API int32_t aver_net_udp_local_port(int32_t s) {
    SockEntry* e = sock(s, Kind::Udp);
    return e ? e->udp.localAddress().port : 0;
}

AVER_NET_API int32_t aver_net_udp_send(int32_t s, const uint8_t* data, int32_t length, const char* ip, int32_t port) {
    SockEntry* e = sock(s, Kind::Udp);
    Address a;
    if (!e || length < 0 || !parseAddr(ip, port, a)) return AVER_NET_ERROR;
    return e->udp.sendTo(a, data, static_cast<usize>(length));
}

AVER_NET_API int32_t aver_net_udp_recv(int32_t s, uint8_t* buffer, int32_t capacity, uint8_t* ipOut, int32_t ipCapacity, int32_t* portOut) {
    SockEntry* e = sock(s, Kind::Udp);
    if (!e || capacity < 0) return AVER_NET_ERROR;
    Address from;
    const int n = e->udp.recvFrom(from, buffer, static_cast<usize>(capacity));
    if (n >= 0) putAddr(from, ipOut, ipCapacity, portOut);
    return n;
}

AVER_NET_API int32_t aver_net_udp_wait(int32_t s, int32_t timeoutMs) {
    SockEntry* e = sock(s, Kind::Udp);
    return e && e->udp.wait(timeoutMs) ? 1 : 0;
}

AVER_NET_API int32_t aver_net_udp_set_broadcast(int32_t s, int32_t enable) {
    SockEntry* e = sock(s, Kind::Udp);
    return e && e->udp.setBroadcast(enable != 0) ? 1 : 0;
}

// ---- TCP ----

AVER_NET_API int32_t aver_net_tcp_connect(const char* ip, int32_t port) {
    Address a;
    if (!parseAddr(ip, port, a)) return 0;
    auto e = std::make_unique<SockEntry>();
    e->kind = Kind::Tcp;
    // A loopback target is reached from the loopback address: no implicit wildcard bind.
    Address from = a.family == 6 ? Address::loopback6(0) : Address::loopback4(0);
    if (!e->tcp.beginConnect(a, a.isLoopback() ? &from : nullptr)) return 0;
    return addSock(std::move(e));
}

AVER_NET_API int32_t aver_net_tcp_status(int32_t s) {
    SockEntry* e = sock(s, Kind::Tcp);
    return e ? e->tcp.connectStatus() : -1;
}

AVER_NET_API int32_t aver_net_tcp_send(int32_t s, const uint8_t* data, int32_t length) {
    SockEntry* e = sock(s, Kind::Tcp);
    if (!e || length < 0) return AVER_NET_ERROR;
    return e->tcp.send(data, static_cast<usize>(length));
}

AVER_NET_API int32_t aver_net_tcp_recv(int32_t s, uint8_t* buffer, int32_t capacity) {
    SockEntry* e = sock(s, Kind::Tcp);
    if (!e || capacity < 0) return AVER_NET_ERROR;
    return e->tcp.recv(buffer, static_cast<usize>(capacity));
}

AVER_NET_API int32_t aver_net_tcp_wait(int32_t s, int32_t forWrite, int32_t timeoutMs) {
    SockEntry* e = sock(s, Kind::Tcp);
    return e && e->tcp.wait(forWrite != 0, timeoutMs) ? 1 : 0;
}

AVER_NET_API int32_t aver_net_tcp_set_nodelay(int32_t s, int32_t enable) {
    SockEntry* e = sock(s, Kind::Tcp);
    return e && e->tcp.setNoDelay(enable != 0) ? 1 : 0;
}

AVER_NET_API int32_t aver_net_tcp_local_port(int32_t s) {
    SockEntry* e = sock(s, Kind::Tcp);
    return e ? e->tcp.localAddress().port : 0;
}

AVER_NET_API int32_t aver_net_tcp_remote(int32_t s, uint8_t* ipOut, int32_t ipCapacity, int32_t* portOut) {
    SockEntry* e = sock(s, Kind::Tcp);
    if (!e) return 0;
    const Address a = e->tcp.peerAddress();
    if (!a.valid()) return 0;
    putAddr(a, ipOut, ipCapacity, portOut);
    return 1;
}

AVER_NET_API int32_t aver_net_tcp_listen(const char* ip, int32_t port, int32_t backlog) {
    Address a;
    if (!bindAddr(ip, port, 4, a)) return 0;
    auto e = std::make_unique<SockEntry>();
    e->kind = Kind::Listener;
    if (!e->listener.listen(a, backlog > 0 ? backlog : 16)) return 0;
    return addSock(std::move(e));
}

AVER_NET_API int32_t aver_net_tcp_accept(int32_t listener) {
    SockEntry* l = sock(listener, Kind::Listener);
    if (!l) return 0;
    auto e = std::make_unique<SockEntry>();
    e->kind = Kind::Tcp;
    Address from;
    if (!l->listener.accept(e->tcp, from)) return 0;
    return addSock(std::move(e));
}

AVER_NET_API int32_t aver_net_tcp_listener_port(int32_t listener) {
    SockEntry* l = sock(listener, Kind::Listener);
    return l ? l->listener.localAddress().port : 0;
}

AVER_NET_API int32_t aver_net_tcp_listener_wait(int32_t listener, int32_t timeoutMs) {
    SockEntry* l = sock(listener, Kind::Listener);
    return l && l->listener.wait(timeoutMs) ? 1 : 0;
}

AVER_NET_API int32_t aver_net_close(int32_t handle) {
    return g_socks.erase(handle) ? 1 : 0;
}

// ---- hosts ----

AVER_NET_API int32_t aver_net_host_create(const char* bindIp, int32_t port, int32_t maxPeers, int32_t channelLimit, int32_t inBandwidth, int32_t outBandwidth) {
    HostConfig cfg;
    cfg.maxPeers = static_cast<u32>(std::clamp(maxPeers, 1, 4095));
    cfg.channelLimit = channelLimit > 0 ? static_cast<u32>(std::min(channelLimit, 255)) : 0;
    cfg.inBandwidth = inBandwidth > 0 ? static_cast<u32>(inBandwidth) : 0;
    cfg.outBandwidth = outBandwidth > 0 ? static_cast<u32>(outBandwidth) : 0;
    std::unique_ptr<DatagramSocket> ds;
    if (bindIp && bindIp[0]) {
        Address a;
        if (!bindAddr(bindIp, port, 4, a)) return 0;
        UdpSocket u;
        if (!u.bind(a)) return 0;
        u.setBufferSizes(1 << 20, 1 << 20);
        ds = std::make_unique<UdpDatagramSocket>(std::move(u));
    }
    auto e = std::make_unique<HostEntry>();
    e->host = std::make_unique<Host>(std::move(ds), cfg);
    const i32 id = g_nextHost++;
    g_hosts[id] = std::move(e);
    return id;
}

AVER_NET_API int32_t aver_net_host_destroy(int32_t host) { return g_hosts.erase(host) ? 1 : 0; }

AVER_NET_API int32_t aver_net_host_local_port(int32_t host) {
    HostEntry* h = hostOf(host);
    return h && h->host->socket() ? h->host->socket()->localAddress().port : 0;
}

AVER_NET_API int32_t aver_net_host_max_peers(int32_t host) {
    HostEntry* h = hostOf(host);
    return h ? static_cast<i32>(h->host->peerSlots()) : 0;
}

AVER_NET_API int32_t aver_net_host_channel_limit(int32_t host, int32_t limit) {
    HostEntry* h = hostOf(host);
    if (!h) return 0;
    h->host->setChannelLimit(limit > 0 ? static_cast<u32>(limit) : 0);
    return 1;
}

AVER_NET_API int32_t aver_net_host_bandwidth_limit(int32_t host, int32_t inBandwidth, int32_t outBandwidth) {
    HostEntry* h = hostOf(host);
    if (!h) return 0;
    h->host->setBandwidthLimit(inBandwidth > 0 ? static_cast<u32>(inBandwidth) : 0, outBandwidth > 0 ? static_cast<u32>(outBandwidth) : 0);
    return 1;
}

AVER_NET_API int32_t aver_net_host_refuse(int32_t host, int32_t refuse) {
    HostEntry* h = hostOf(host);
    if (!h) return 0;
    h->host->setRefuseConnections(refuse != 0);
    return 1;
}

AVER_NET_API int64_t aver_net_host_connect(int32_t host, const char* ip, int32_t port, int32_t channels, uint32_t data) {
    HostEntry* h = hostOf(host);
    Address a;
    if (!h || !parseAddr(ip, port, a) || port == 0) return 0;
    if (!h->host->hasSocket()) {
        UdpSocket u;
        if (!u.open(a.family)) return 0;
        // A loopback target is reached from the loopback address; anything else leaves the OS to pick the interface at the first send.
        if (a.isLoopback() && !u.bind(a.family == 6 ? Address::loopback6(0) : Address::loopback4(0))) return 0;
        u.setBufferSizes(1 << 20, 1 << 20);
        h->host->attachSocket(std::make_unique<UdpDatagramSocket>(std::move(u)));
    }
    Peer* p = h->host->connect(a, channels > 0 ? static_cast<u32>(channels) : 0, data, static_cast<u64>(clockMs()));
    return p ? keyOf(*p) : 0;
}

AVER_NET_API int32_t aver_net_host_service(int32_t host, int32_t timeoutMs, int32_t* type, int64_t* peer, int32_t* channel, uint32_t* data, int32_t* reason, int32_t* length) {
    HostEntry* h = hostOf(host);
    if (!h) return -1;
    const i64 deadline = clockMs() + std::max(timeoutMs, 0);
    for (;;) {
        Event ev;
        if (h->host->service(ev, static_cast<u64>(clockMs())) != 0) {
            h->last = std::move(ev);
            const Event& e = h->last;
            if (type) *type = static_cast<i32>(e.type);
            if (peer) *peer = (static_cast<i64>(e.peerGeneration) << 16) | e.peerIndex;
            if (channel) *channel = e.channel;
            if (data) *data = e.data;
            if (reason) *reason = static_cast<i32>(e.reason);
            if (length) *length = static_cast<i32>(e.packet.size());
            return 1;
        }
        const i64 left = deadline - clockMs();
        if (left <= 0) return 0;
        DatagramSocket* ds = h->host->socket();
        const int slice = static_cast<int>(std::min<i64>(left, 5));
        if (ds && ds->native() != -1) ds->wait(slice);
        else std::this_thread::sleep_for(std::chrono::milliseconds(slice));
    }
}

AVER_NET_API int32_t aver_net_host_event_data(int32_t host, uint8_t* buffer, int32_t capacity) {
    HostEntry* h = hostOf(host);
    if (!h || !buffer || capacity < 0) return 0;
    const i32 n = std::min<i32>(capacity, static_cast<i32>(h->last.packet.size()));
    if (n > 0) std::memcpy(buffer, h->last.packet.data(), static_cast<usize>(n));
    return n;
}

AVER_NET_API int32_t aver_net_host_flush(int32_t host) {
    HostEntry* h = hostOf(host);
    if (!h) return 0;
    h->host->flush(static_cast<u64>(clockMs()));
    return 1;
}

AVER_NET_API int32_t aver_net_host_broadcast(int32_t host, int32_t channel, const uint8_t* data, int32_t length, int32_t flags) {
    HostEntry* h = hostOf(host);
    if (!h || length < 0 || channel < 0 || channel > 254) return 0;
    h->host->broadcast(static_cast<u8>(channel), data, static_cast<usize>(length), static_cast<u32>(flags));
    return 1;
}

AVER_NET_API int32_t aver_net_host_socket_send(int32_t host, const char* ip, int32_t port, const uint8_t* data, int32_t length) {
    HostEntry* h = hostOf(host);
    Address a;
    if (!h || length < 0 || !parseAddr(ip, port, a)) return 0;
    return h->host->socketSend(a, data, static_cast<usize>(length)) ? 1 : 0;
}

AVER_NET_API int32_t aver_net_host_peers(int32_t host, int64_t* keys, int32_t capacity) {
    HostEntry* h = hostOf(host);
    if (!h) return 0;
    i32 n = 0;
    for (u32 i = 0; i < h->host->peerSlots(); ++i) {
        Peer* p = h->host->peerAt(i);
        if (!p || p->state() != PeerState::Connected) continue;
        if (keys && n < capacity) keys[n] = keyOf(*p);
        ++n;
    }
    return n;
}

AVER_NET_API double aver_net_host_stat(int32_t host, int32_t which) {
    HostEntry* h = hostOf(host);
    if (!h) return 0;
    const HostStats& s = h->host->stats();
    switch (which) {
        case AVER_NET_HOSTSTAT_SENT_BYTES: return static_cast<double>(s.sentBytes);
        case AVER_NET_HOSTSTAT_SENT_PACKETS: return static_cast<double>(s.sentPackets);
        case AVER_NET_HOSTSTAT_RECEIVED_BYTES: return static_cast<double>(s.receivedBytes);
        case AVER_NET_HOSTSTAT_RECEIVED_PACKETS: return static_cast<double>(s.receivedPackets);
        case AVER_NET_HOSTSTAT_BAD_CHECKSUM: return static_cast<double>(s.badChecksum);
        default: return 0;
    }
}

// ---- peers ----

AVER_NET_API int32_t aver_net_peer_send(int32_t host, int64_t peer, int32_t channel, const uint8_t* data, int32_t length, int32_t flags) {
    Peer* p = peerOf(host, peer);
    if (!p || length < 0) return AVER_NET_SEND_BAD_HANDLE;
    if (channel < 0 || channel > 254) return AVER_NET_SEND_BAD_CHANNEL;
    return p->send(static_cast<u8>(channel), data, static_cast<usize>(length), static_cast<u32>(flags));
}

AVER_NET_API int32_t aver_net_peer_state(int32_t host, int64_t peer) {
    Peer* p = peerOf(host, peer);
    return p ? static_cast<i32>(p->state()) : 0;
}

AVER_NET_API int32_t aver_net_peer_disconnect(int32_t host, int64_t peer, uint32_t data, int32_t mode) {
    Peer* p = peerOf(host, peer);
    if (!p) return 0;
    if (mode == AVER_NET_DISCONNECT_MODE_NOW) p->disconnectNow(data);
    else if (mode == AVER_NET_DISCONNECT_MODE_LATER) p->disconnectLater(data);
    else p->disconnect(data);
    return 1;
}

AVER_NET_API int32_t aver_net_peer_reset(int32_t host, int64_t peer) {
    Peer* p = peerOf(host, peer);
    if (!p) return 0;
    p->reset();
    return 1;
}

AVER_NET_API int32_t aver_net_peer_ping(int32_t host, int64_t peer) {
    Peer* p = peerOf(host, peer);
    if (!p) return 0;
    p->ping();
    return 1;
}

AVER_NET_API int32_t aver_net_peer_set_ping_interval(int32_t host, int64_t peer, int32_t intervalMs) {
    Peer* p = peerOf(host, peer);
    if (!p) return 0;
    p->setPingInterval(intervalMs > 0 ? static_cast<u32>(intervalMs) : 0);
    return 1;
}

AVER_NET_API int32_t aver_net_peer_set_timeout(int32_t host, int64_t peer, int32_t limit, int32_t minimumMs, int32_t maximumMs) {
    Peer* p = peerOf(host, peer);
    if (!p) return 0;
    p->setTimeouts(limit > 0 ? static_cast<u32>(limit) : 0, minimumMs > 0 ? static_cast<u32>(minimumMs) : 0, maximumMs > 0 ? static_cast<u32>(maximumMs) : 0);
    return 1;
}

AVER_NET_API int32_t aver_net_peer_throttle(int32_t host, int64_t peer, int32_t intervalMs, int32_t acceleration, int32_t deceleration) {
    Peer* p = peerOf(host, peer);
    if (!p) return 0;
    p->throttleConfigure(static_cast<u32>(std::max(intervalMs, 0)), static_cast<u32>(std::max(acceleration, 0)), static_cast<u32>(std::max(deceleration, 0)));
    return 1;
}

AVER_NET_API int32_t aver_net_peer_address(int32_t host, int64_t peer, uint8_t* ipOut, int32_t ipCapacity, int32_t* portOut) {
    Peer* p = peerOf(host, peer);
    if (!p) return 0;
    putAddr(p->address(), ipOut, ipCapacity, portOut);
    return 1;
}

AVER_NET_API uint32_t aver_net_peer_connect_data(int32_t host, int64_t peer) {
    Peer* p = peerOf(host, peer);
    return p ? p->connectData() : 0;
}

AVER_NET_API double aver_net_peer_stat(int32_t host, int64_t peer, int32_t which) {
    Peer* p = peerOf(host, peer);
    if (!p) return 0;
    u32 interval = 0, accel = 0, decel = 0;
    p->getThrottle(interval, accel, decel);
    switch (which) {
        case AVER_NET_PEERSTAT_PACKET_LOSS: return p->packetLoss();
        case AVER_NET_PEERSTAT_PACKET_LOSS_VARIANCE: return p->packetLossVariance();
        case AVER_NET_PEERSTAT_PACKET_THROTTLE: return 32;                 // ENet's full throttle; Aver applies none
        case AVER_NET_PEERSTAT_ROUND_TRIP_TIME: return p->roundTripTime();
        case AVER_NET_PEERSTAT_ROUND_TRIP_TIME_VARIANCE: return p->roundTripTimeVariance();
        case AVER_NET_PEERSTAT_LAST_ROUND_TRIP_TIME: return p->lastRoundTripTime();
        case AVER_NET_PEERSTAT_LAST_ROUND_TRIP_TIME_VARIANCE: return p->roundTripTimeVariance();
        case AVER_NET_PEERSTAT_PACKET_LOSS_EPOCH: return 0;
        case AVER_NET_PEERSTAT_PACKET_THROTTLE_EPOCH: return 0;
        case AVER_NET_PEERSTAT_PACKET_THROTTLE_ACCELERATION: return accel;
        case AVER_NET_PEERSTAT_PACKET_THROTTLE_DECELERATION: return decel;
        case AVER_NET_PEERSTAT_PACKET_THROTTLE_INTERVAL: return interval;
        case AVER_NET_PEERSTAT_BYTES_SENT: return static_cast<double>(p->bytesSent());
        case AVER_NET_PEERSTAT_BYTES_RECEIVED: return static_cast<double>(p->bytesReceived());
        case AVER_NET_PEERSTAT_PACKETS_SENT: return static_cast<double>(p->packetsSent());
        case AVER_NET_PEERSTAT_PACKETS_LOST: return static_cast<double>(p->packetsLost());
        case AVER_NET_PEERSTAT_PACKETS_DROPPED: return static_cast<double>(p->packetsDropped());
        case AVER_NET_PEERSTAT_MTU: return p->mtu();
        case AVER_NET_PEERSTAT_LOWEST_ROUND_TRIP_TIME: return p->lowestRoundTripTime();
        case AVER_NET_PEERSTAT_RELIABLE_IN_TRANSIT: return static_cast<double>(p->reliableDataInTransit());
        case AVER_NET_PEERSTAT_PING_INTERVAL: return p->pingInterval();
        case AVER_NET_PEERSTAT_CHANNELS: return p->channelCount();
        default: return 0;
    }
}

} // extern "C"
