// The OS socket layer and the protocol over real sockets. LOOPBACK ONLY: every bind is 127.0.0.1 or ::1 with port 0 (the OS picks), nothing
// binds a wildcard (that pops the Windows Firewall dialog), every wait is short, and the only name resolved is "localhost".
#include "aver/core/Log.hpp"
#include "aver/net/Protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace aver;
using namespace aver::net;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static u64 nowMs() {
    static const auto t0 = std::chrono::steady_clock::now();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()) + 1;
}

static std::vector<u8> pattern(size_t n, u32 seed) {
    std::vector<u8> v(n);
    u32 s = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) { s = s * 1664525u + 1013904223u; v[i] = static_cast<u8>(s >> 24); }
    return v;
}

static void testAddress() {
    AVER_INFO("addresses");
    Address a;
    check(Address::parse("127.0.0.1", 80, a) && a.family == 4 && a.port == 80 && a.isLoopback() && a.ipString() == "127.0.0.1", "IPv4 text round trips");
    check(Address::parse("::1", 9, a) && a.family == 6 && a.isLoopback() && a.ipString() == "::1" && a.toString() == "[::1]:9", "IPv6 text round trips");
    check(Address::parse("[2001:db8::7]", 1, a) && a.family == 6 && a.ipString() == "2001:db8::7" && !a.isLoopback(), "bracketed IPv6");
    check(!Address::parse("localhost", 1, a) && !Address::parse("999.1.1.1", 1, a) && !Address::parse("", 1, a) && !Address::parse("1.2.3", 1, a), "names and malformed text are not numeric addresses");
    check(Address::parse("0.0.0.0", 0, a) && a.isUnspecified(), "unspecified");
    check(Address::parse("fe80::1%7", 1, a) && a.scope == 7 && Address::parse("fe80::1%4294967295", 1, a) && a.scope == 4294967295u, "a numeric zone id up to 32 bits is kept");
    check(!Address::parse("fe80::1%4294967296", 1, a) && !Address::parse("fe80::1%99999999999999999999", 1, a), "a numeric zone id past 32 bits is refused, not thrown");
    std::vector<Address> v;
    check(resolve("127.0.0.1", 0, v) && v.size() == 1 && v[0].isLoopback(), "resolve takes numeric text without a lookup");
    check(resolve("localhost", 4, v) && !v.empty() && v[0].family == 4 && v[0].isLoopback(), "localhost resolves to a loopback IPv4 address");
    check(!resolve("127.0.0.1", 6, v), "an IPv4 address does not resolve as IPv6");
    const auto local = localAddresses();
    check(!local.empty() && std::find(local.begin(), local.end(), "127.0.0.1") != local.end(), "the local address list holds the loopback address");
}

static void testUdp() {
    AVER_INFO("UDP over loopback");
    UdpSocket a, b;
    check(a.bind(Address::loopback4(0)) && b.bind(Address::loopback4(0)), "two sockets bind 127.0.0.1:0");
    const Address aa = a.localAddress(), ba = b.localAddress();
    check(aa.port != 0 && ba.port != 0 && aa.port != ba.port && aa.isLoopback(), "the OS picked distinct ports");
    u8 buf[2048];
    Address from;
    check(a.recvFrom(from, buf, sizeof(buf)) == kWouldBlock, "an empty socket would block");
    const auto t0 = nowMs();
    check(!a.wait(30), "wait() times out when nothing arrives");
    check(nowMs() - t0 >= 20, "and it really waited");
    const auto msg = pattern(1200, 1);
    check(a.sendTo(ba, msg.data(), msg.size()) == 1200, "send");
    check(b.wait(2000), "the receiver wakes");
    const int n = b.recvFrom(from, buf, sizeof(buf));
    check(n == 1200 && std::memcmp(buf, msg.data(), 1200) == 0 && from == aa, "receive: bytes and source address");
    check(a.sendTo(ba, buf, 0) == 0 && b.wait(2000) && b.recvFrom(from, buf, sizeof(buf)) == 0, "an empty datagram is a datagram");
    UdpSocket c;
    check(!c.bind(ba), "a second bind to a taken port fails");
    // datagram to a closed port must not poison the sender
    UdpSocket dead;
    dead.bind(Address::loopback4(0));
    const Address deadAddr = dead.localAddress();
    dead.close();
    a.sendTo(deadAddr, msg.data(), 10);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(a.recvFrom(from, buf, sizeof(buf)) == kWouldBlock, "a datagram to a closed port leaves the sender usable");
    check(b.sendTo(aa, msg.data(), 5) == 5 && a.wait(2000) && a.recvFrom(from, buf, sizeof(buf)) == 5, "and it still receives");

    UdpSocket v6a, v6b;
    if (v6a.bind(Address::loopback6(0)) && v6b.bind(Address::loopback6(0))) {
        const Address b6 = v6b.localAddress();
        check(v6a.sendTo(b6, msg.data(), 64) == 64 && v6b.wait(2000) && v6b.recvFrom(from, buf, sizeof(buf)) == 64 && from.family == 6, "IPv6 loopback datagram");
        check(a.sendTo(b6, msg.data(), 1) < 0, "a v4 socket refuses a v6 destination");
    } else {
        AVER_INFO("  note  no IPv6 loopback on this machine: the IPv6 UDP checks were skipped");
    }
}

static void testTcp() {
    AVER_INFO("TCP over loopback");
    TcpListener lis;
    check(lis.listen(Address::loopback4(0), 4), "listen on 127.0.0.1:0");
    const Address la = lis.localAddress();
    check(la.port != 0, "the listener has a port");
    TcpSocket none;
    Address from;
    check(!lis.accept(none, from), "nothing to accept yet");
    TcpSocket cli;
    check(cli.beginConnect(la), "a non-blocking connect starts");
    check(lis.wait(2000), "the listener wakes");
    TcpSocket srv;
    check(lis.accept(srv, from) && from.isLoopback(), "accept");
    check(cli.wait(true, 2000), "the client becomes writable");
    check(cli.connectStatus() == 1, "connectStatus: connected");
    check(cli.peerAddress() == la && srv.peerAddress() == cli.localAddress(), "peer addresses agree");
    cli.setNoDelay(true);

    const auto msg = pattern(5000, 2);
    check(cli.send(msg.data(), msg.size()) == 5000, "send 5000 bytes");
    std::vector<u8> got;
    u8 buf[4096];
    for (int i = 0; i < 100 && got.size() < 5000; ++i) {
        srv.wait(false, 50);
        const int n = srv.recv(buf, sizeof(buf));
        if (n > 0) got.insert(got.end(), buf, buf + n);
    }
    check(got == msg, "the stream arrives intact (TCP may split it)");
    check(srv.recv(buf, sizeof(buf)) == kWouldBlock, "no data means would-block");

    // a megabyte through a socket buffer that takes it in pieces
    const auto big = pattern(1u << 20, 3);
    size_t sent = 0;
    std::vector<u8> rx;
    for (int i = 0; i < 20000 && (sent < big.size() || rx.size() < big.size()); ++i) {
        if (sent < big.size()) {
            const int n = srv.send(big.data() + sent, std::min<size_t>(big.size() - sent, 65536));
            if (n > 0) sent += static_cast<size_t>(n);
        }
        const int n = cli.recv(buf, sizeof(buf));
        if (n > 0) rx.insert(rx.end(), buf, buf + n);
        else if (n == kWouldBlock && sent >= big.size()) cli.wait(false, 5);
    }
    check(rx == big, "1 MB streams through partial sends and receives");

    srv.close();
    int r = kWouldBlock;
    for (int i = 0; i < 100 && r == kWouldBlock; ++i) { cli.wait(false, 20); r = cli.recv(buf, sizeof(buf)); }
    check(r == kSockClosed, "an orderly close reads as kSockClosed");

    TcpSocket refused;
    {
        TcpListener gone;
        gone.listen(Address::loopback4(0), 1);
        const Address ga = gone.localAddress();
        gone.close();
        refused.beginConnect(ga);
        int st = 0;
        for (int i = 0; i < 100 && st == 0; ++i) { refused.wait(true, 20); st = refused.connectStatus(); }
        check(st == -1, "connecting to a closed port reports failure");
    }

    TcpListener l6;
    if (l6.listen(Address::loopback6(0), 1)) {
        TcpSocket c6, s6;
        c6.beginConnect(l6.localAddress());
        l6.wait(2000);
        check(l6.accept(s6, from) && from.family == 6, "IPv6 loopback accept");
    } else {
        AVER_INFO("  note  no IPv6 loopback on this machine: the IPv6 TCP check was skipped");
    }
}

struct RealNode {
    std::unique_ptr<Host> host;
    Address addr;
    std::vector<Event> events;
    explicit RealNode(HostConfig cfg = {}) {
        UdpSocket s;
        s.bind(Address::loopback4(0));
        addr = s.localAddress();
        host = std::make_unique<Host>(std::make_unique<UdpDatagramSocket>(std::move(s)), cfg);
    }
    size_t count(EventType t) const { size_t n = 0; for (const Event& e : events) n += e.type == t; return n; }
    void pump() { Event ev; while (host->service(ev, nowMs()) != 0) events.push_back(std::move(ev)); }
};

static bool runUntil(std::vector<RealNode*> nodes, const std::function<bool()>& done, int limitMs = 5000) {
    const u64 end = nowMs() + static_cast<u64>(limitMs);
    while (nowMs() < end) {
        for (RealNode* n : nodes) n->pump();
        if (done()) return true;
        nodes[0]->host->socket()->wait(2);
    }
    for (RealNode* n : nodes) n->pump();
    return done();
}

static void testHostOverSockets() {
    AVER_INFO("the protocol over real loopback sockets");
    RealNode server, client;
    check(server.addr.isLoopback() && client.addr.isLoopback(), "both hosts are bound to loopback");
    Peer* cp = client.host->connect(server.addr, 3, 0xABCD, nowMs());
    check(cp && runUntil({&server, &client}, [&] { return server.count(EventType::Connect) == 1 && client.count(EventType::Connect) == 1; }), "handshake");
    check(server.count(EventType::Connect) == 1 && server.events[0].data == 0xABCD, "connect data");
    Peer* sp = server.host->peerAt(server.events[0].peerIndex);
    check(sp && sp->address().isLoopback(), "the server peer is the client's loopback address");

    for (u32 i = 0; i < 300; ++i) { u8 b[4] = {u8(i), u8(i >> 8), 0, 0}; cp->send(i % 3, b, 4, kFlagReliable); }
    auto big = pattern(300000, 9);
    cp->send(1, big.data(), big.size(), kFlagReliable);
    check(runUntil({&server, &client}, [&] { return server.count(EventType::Receive) >= 301; }, 8000), "300 small packets and a 300 KB packet arrive");
    u32 next[3] = {0, 1, 2};
    bool ordered = true, bigOk = false;
    for (const Event& e : server.events) {
        if (e.type != EventType::Receive) continue;
        if (e.packet.size() == 300000) { bigOk = e.packet == big; continue; }
        const u32 v = e.packet[0] | (e.packet[1] << 8);
        if (v != next[e.channel]) ordered = false;
        next[e.channel] += 3;
    }
    check(ordered, "in order per channel over a real socket");
    check(bigOk, "the 300 KB packet is intact");
    check(cp->roundTripTime() < 200, "RTT on loopback is small (" + std::to_string(cp->roundTripTime()) + " ms)");

    sp->send(0, big.data(), 100, kFlagUnsequenced);
    check(runUntil({&server, &client}, [&] { return client.count(EventType::Receive) >= 1; }), "an unsequenced packet back");

    cp->disconnect(42);
    check(runUntil({&server, &client}, [&] { return server.count(EventType::Disconnect) == 1 && client.count(EventType::Disconnect) == 1; }), "graceful disconnect over the socket");
    check(server.events.back().type == EventType::Disconnect && server.events.back().data == 42, "with its data word");

    // the same pair of sockets under a tiny MTU forces many fragments
    HostConfig small; small.mtu = 600;
    RealNode s2(small), c2(small);
    Peer* cp2 = c2.host->connect(s2.addr, 1, 0, nowMs());
    runUntil({&s2, &c2}, [&] { return s2.count(EventType::Connect) == 1 && c2.count(EventType::Connect) == 1; });
    const auto b2 = pattern(100000, 4);
    if (cp2) cp2->send(0, b2.data(), b2.size(), kFlagReliable);
    runUntil({&s2, &c2}, [&] { return s2.count(EventType::Receive) == 1; }, 8000);
    check(s2.count(EventType::Receive) == 1 && s2.events.back().packet == b2, "a 600 byte MTU fragments 100 KB and reassembles it");
}

int main() {
    AVER_INFO("NetSocketTest");
    testAddress();
    testUdp();
    testTcp();
    testHostOverSockets();
    if (g_failures) { AVER_ERROR("{} check(s) failed", g_failures); return 1; }
    AVER_INFO("all checks passed");
    return 0;
}
