// The reliable-UDP host/peer protocol on SimNetwork: handshake, reliability and order under loss and reordering, unreliable-ordered and
// unsequenced delivery, fragmentation, channels, timeouts and disconnects, bandwidth limits, ping and RTT, hostile datagrams.
#include "aver/core/Log.hpp"
#include "aver/net/Protocol.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::net;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

struct Rec {
    EventType type;
    u16 peer;
    u8 channel;
    u32 data;
    DisconnectReason reason;
    std::vector<u8> packet;
    u64 at;
};

struct Node {
    std::unique_ptr<Host> host;
    Address addr;
    std::vector<Rec> log;
    std::vector<Rec> of(EventType t) const {
        std::vector<Rec> r;
        for (const Rec& e : log) if (e.type == t) r.push_back(e);
        return r;
    }
    size_t count(EventType t) const { return of(t).size(); }
};

struct Rig {
    SimNetwork net;
    std::vector<std::unique_ptr<Node>> nodes;
    explicit Rig(u64 seed = 7) : net(seed) { net.setNow(1000); }

    Node& add(u16 port, HostConfig cfg = {}) {
        auto n = std::make_unique<Node>();
        n->addr = Address::loopback4(port);
        cfg.seed = 1000 + port;
        n->host = std::make_unique<Host>(net.bind(n->addr), cfg);
        nodes.push_back(std::move(n));
        return *nodes.back();
    }
    void pump() {
        for (auto& n : nodes) {
            Event ev;
            while (n->host->service(ev, net.now()) != 0) {
                n->log.push_back(Rec{ev.type, ev.peerIndex, ev.channel, ev.data, ev.reason, std::move(ev.packet), net.now()});
                ev = Event{};
            }
        }
    }
    // Steps virtual time until done() or `limitMs` passes. Returns whether done() became true.
    bool run(const std::function<bool()>& done, u64 limitMs = 60000, u32 stepMs = 2) {
        const u64 end = net.now() + limitMs;
        while (net.now() < end) {
            pump();
            if (done()) return true;
            net.advance(stepMs);
        }
        pump();
        return done();
    }
    void settle(u64 ms) { run([] { return false; }, ms); }
};

static std::vector<u8> pattern(size_t n, u32 seed) {
    std::vector<u8> v(n);
    u32 s = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) { s = s * 1664525u + 1013904223u; v[i] = static_cast<u8>(s >> 24); }
    return v;
}
static std::vector<u8> u32pkt(u32 v) { return {u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24)}; }
static u32 pktU32(const std::vector<u8>& p) { return p.size() >= 4 ? (u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24) : 0xFFFFFFFFu; }

// Connects client -> server; both Connected when it returns true.
static Peer* connectPair(Rig& r, Node& client, Node& server, u32 channels = 2, u32 data = 77) {
    Peer* cp = client.host->connect(server.addr, channels, data, r.net.now());
    if (!cp) return nullptr;
    const bool ok = r.run([&] { return client.count(EventType::Connect) >= 1 && server.count(EventType::Connect) >= 1; }, 30000);
    return ok ? cp : nullptr;
}
static Peer* serverPeer(Node& server, size_t i = 0) {
    const auto c = server.of(EventType::Connect);
    return i < c.size() ? server.host->peerAt(c[i].peer) : nullptr;
}

static void testCrc() {
    AVER_INFO("crc32");
    const char* s = "123456789";
    check(crc32(reinterpret_cast<const u8*>(s), 9) == 0xCBF43926u, "the CRC-32 check value");
}

static void testHandshake() {
    AVER_INFO("handshake");
    Rig r;
    r.net.config.minDelayMs = r.net.config.maxDelayMs = 20;
    HostConfig sc; sc.channelLimit = 2; sc.mtu = 1000;
    Node& server = r.add(5000, sc);
    Node& client = r.add(5001);
    Peer* cp = client.host->connect(server.addr, 8, 0xCAFEBABE, r.net.now());
    check(cp && cp->state() == PeerState::Connecting, "connect() leaves the peer Connecting");
    check(r.run([&] { return client.count(EventType::Connect) && server.count(EventType::Connect); }), "both sides get a Connect event");
    const Rec sc0 = server.of(EventType::Connect)[0];
    check(sc0.data == 0xCAFEBABE, "the server sees the connect-data word");
    Peer* sp = serverPeer(server);
    check(sp && sp->state() == PeerState::Connected && cp->state() == PeerState::Connected, "both peers are Connected");
    check(cp->channelCount() == 2 && sp->channelCount() == 2, "the channel count is the smaller of the request and the server's limit");
    check(cp->mtu() == 1000 && sp->mtu() == 1000, "the mtu is the smaller of the two");
    check(sp->address() == client.addr && cp->address() == server.addr, "peers know each other's address");
    check(server.host->connectedCount() == 1 && client.host->connectedCount() == 1, "connectedCount");
    check(client.of(EventType::Connect)[0].data == 0, "the client's Connect carries no data word");
    // a second connect from the same client to the same server is a new peer slot
    check(cp->index() == 0 && sp->index() == 0, "peer ids are slot indices");
}

static void testHandshakeUnderLoss() {
    AVER_INFO("handshake under loss, duplication and reordering");
    for (u64 seed = 1; seed <= 12; ++seed) {
        Rig r(seed);
        r.net.config.lossProbability = 0.4;
        r.net.config.duplicateProbability = 0.3;
        r.net.config.minDelayMs = 5; r.net.config.maxDelayMs = 80;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        bool ok = cp && server.count(EventType::Connect) == 1 && client.count(EventType::Connect) == 1;
        if (!ok) check(false, "seed " + std::to_string(seed) + " connects exactly once");
    }
    check(true, "twelve seeds connect, each with one Connect event per side");
}

static void testReliableOrder() {
    AVER_INFO("reliable delivery in order under 30% loss and reordering");
    Rig r(11);
    r.net.config.lossProbability = 0.3;
    r.net.config.duplicateProbability = 0.1;
    r.net.config.minDelayMs = 10; r.net.config.maxDelayMs = 120;      // a wide range: datagrams overtake each other
    Node& server = r.add(5000);
    Node& client = r.add(5001);
    Peer* cp = connectPair(r, client, server);
    check(cp != nullptr, "connected through the loss");
    if (!cp) return;
    const u32 N = 600;
    for (u32 i = 0; i < N; ++i) {
        auto p = u32pkt(i);
        cp->send(i % 2, p.data(), p.size(), kFlagReliable);
    }
    const bool done = r.run([&] { return server.count(EventType::Receive) >= N; }, 600000);
    check(done, "all 600 packets arrive");
    u32 next[2] = {0, 1};
    bool ordered = true, once = true;
    size_t got = 0;
    for (const Rec& e : server.of(EventType::Receive)) {
        ++got;
        if (e.channel > 1 || pktU32(e.packet) != next[e.channel]) { ordered = false; break; }
        next[e.channel] += 2;
    }
    once = got == N;
    check(ordered, "each channel delivers its packets in send order");
    bool flagged = true;
    for (const Rec& e : server.of(EventType::Receive)) flagged &= e.data == kFlagReliable;
    check(flagged, "and every Receive event reports the reliable flag");
    check(once, "no duplicates and no losses: exactly N packets");
    check(cp->packetsLost() > 0, "the sender had to resend (packetsLost counted)");
    check(cp->packetLoss() <= kPacketLossScale, "packet loss estimate is a fraction");
}

static void testReliableBothWays() {
    AVER_INFO("reliable traffic in both directions at once");
    Rig r(5);
    r.net.config.lossProbability = 0.25;
    r.net.config.minDelayMs = 5; r.net.config.maxDelayMs = 50;
    Node& server = r.add(5000);
    Node& client = r.add(5001);
    Peer* cp = connectPair(r, client, server);
    if (!cp) { check(false, "connected"); return; }
    Peer* sp = serverPeer(server);
    for (u32 i = 0; i < 200; ++i) {
        auto a = u32pkt(i), b = u32pkt(1000 + i);
        cp->send(0, a.data(), a.size(), kFlagReliable);
        sp->send(1, b.data(), b.size(), kFlagReliable);
    }
    check(r.run([&] { return server.count(EventType::Receive) >= 200 && client.count(EventType::Receive) >= 200; }, 300000), "both directions complete");
    bool a = true, b = true;
    u32 i = 0;
    for (const Rec& e : server.of(EventType::Receive)) a &= pktU32(e.packet) == i++;
    i = 0;
    for (const Rec& e : client.of(EventType::Receive)) b &= pktU32(e.packet) == 1000 + i++;
    check(a && b, "in order both ways");
}

static void testUnreliableOrdered() {
    AVER_INFO("unreliable ordered drops stale packets");
    {
        Rig r;
        r.net.config.minDelayMs = r.net.config.maxDelayMs = 10;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        if (!cp) { check(false, "connected"); return; }
        // first packet takes the slow road, the second the fast one: the second arrives first and the first is then stale
        LinkConfig slow; slow.minDelayMs = slow.maxDelayMs = 300;
        LinkConfig fast; fast.minDelayMs = fast.maxDelayMs = 10;
        r.net.setLink(client.addr, server.addr, slow);
        auto p1 = u32pkt(1), p2 = u32pkt(2);
        cp->send(0, p1.data(), p1.size(), 0);
        client.host->flush(r.net.now());
        r.net.setLink(client.addr, server.addr, fast);
        cp->send(0, p2.data(), p2.size(), 0);
        client.host->flush(r.net.now());
        r.settle(1500);
        const auto rx = server.of(EventType::Receive);
        check(rx.size() == 1 && pktU32(rx[0].packet) == 2, "only the newer packet is delivered; the stale one is dropped");
        check(rx.size() == 1 && rx[0].data == 0, "a Receive event of an unreliable ordered packet carries no flags");
    }
    {
        Rig r(3);
        r.net.config.minDelayMs = 5; r.net.config.maxDelayMs = 150;
        r.net.config.duplicateProbability = 0.2;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        if (!cp) { check(false, "connected"); return; }
        for (u32 i = 1; i <= 300; ++i) {
            auto p = u32pkt(i);
            cp->send(0, p.data(), p.size(), 0);
            r.net.advance(3);
            r.pump();
        }
        r.settle(1500);
        u32 last = 0;
        bool mono = true;
        const auto rx = server.of(EventType::Receive);
        for (const Rec& e : rx) { const u32 v = pktU32(e.packet); if (v <= last) mono = false; last = v; }
        check(mono, "under jitter and duplication the delivered sequence only ever increases");
        check(rx.size() < 300 && rx.size() > 10, "stale packets were dropped, the rest arrived (" + std::to_string(rx.size()) + " of 300)");
    }
}

static void testUnsequenced() {
    AVER_INFO("unsequenced packets are not ordered and not duplicated");
    Rig r;
    r.net.config.minDelayMs = r.net.config.maxDelayMs = 10;
    Node& server = r.add(5000);
    Node& client = r.add(5001);
    Peer* cp = connectPair(r, client, server);
    if (!cp) { check(false, "connected"); return; }
    LinkConfig slow; slow.minDelayMs = slow.maxDelayMs = 300;
    LinkConfig fast; fast.minDelayMs = fast.maxDelayMs = 10;
    r.net.setLink(client.addr, server.addr, slow);
    auto p1 = u32pkt(1), p2 = u32pkt(2);
    cp->send(0, p1.data(), p1.size(), kFlagUnsequenced);
    client.host->flush(r.net.now());
    r.net.setLink(client.addr, server.addr, fast);
    cp->send(0, p2.data(), p2.size(), kFlagUnsequenced);
    client.host->flush(r.net.now());
    r.settle(1500);
    const auto rx = server.of(EventType::Receive);
    check(rx.size() == 2 && pktU32(rx[0].packet) == 2 && pktU32(rx[1].packet) == 1, "both arrive, in the order the wire gave them");
    check(rx.size() == 2 && rx[0].data == kFlagUnsequenced && rx[1].data == kFlagUnsequenced, "and a Receive event reports the unsequenced flag");

    Rig r2(9);
    r2.net.config.duplicateProbability = 0.9;
    r2.net.config.minDelayMs = 5; r2.net.config.maxDelayMs = 40;
    Node& s2 = r2.add(5000);
    Node& c2 = r2.add(5001);
    Peer* cp2 = connectPair(r2, c2, s2);
    if (!cp2) { check(false, "connected for the duplicate run"); return; }
    for (u32 i = 0; i < 100; ++i) { auto p = u32pkt(i); cp2->send(1, p.data(), p.size(), kFlagUnsequenced); r2.net.advance(2); r2.pump(); }
    r2.settle(2000);
    std::vector<u32> seen;
    for (const Rec& e : s2.of(EventType::Receive)) seen.push_back(pktU32(e.packet));
    std::sort(seen.begin(), seen.end());
    check(std::adjacent_find(seen.begin(), seen.end()) == seen.end(), "duplicated datagrams are delivered once");
    check(r2.net.stats.duplicated > 20, "the wire really duplicated");
}

static void testFragmentation() {
    AVER_INFO("fragmentation and reassembly");
    {
        Rig r(21);
        r.net.config.minDelayMs = r.net.config.maxDelayMs = 25;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        if (!cp) { check(false, "connected"); return; }
        const auto big = pattern(1u << 20, 4);
        check(cp->send(0, big.data(), big.size(), kFlagReliable) == kSendOk, "a 1 MB reliable send is accepted");
        check(r.run([&] { return server.count(EventType::Receive) >= 1; }, 120000), "it arrives");
        const auto rx = server.of(EventType::Receive);
        check(rx.size() == 1 && rx[0].packet == big, "reassembled byte for byte, as one packet");
    }
    {
        Rig r(22);
        r.net.config.lossProbability = 0.15;
        r.net.config.duplicateProbability = 0.1;
        r.net.config.minDelayMs = 10; r.net.config.maxDelayMs = 90;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        if (!cp) { check(false, "connected"); return; }
        // ENet's backoff doubles each resend, so a megabyte of fragments over a lossy wire wants the patient timeouts a game would set for a bulk transfer.
        cp->setTimeouts(128, 10000, 3600000);
        serverPeer(server)->setTimeouts(128, 10000, 3600000);
        const auto big = pattern(1u << 20, 5);
        const auto small = pattern(3000, 6);
        cp->send(0, big.data(), big.size(), kFlagReliable);
        cp->send(0, small.data(), small.size(), kFlagReliable);
        const bool arrived = r.run([&] { return server.count(EventType::Receive) >= 2; }, 1800000);
        if (!arrived) {
            AVER_INFO("  diag  received {} state {} inTransit {} client disc {} server disc {} t+{}", server.count(EventType::Receive), static_cast<int>(cp->state()),
                      cp->reliableDataInTransit(), client.count(EventType::Disconnect), server.count(EventType::Disconnect), r.net.now() - 1000);
            for (const Rec& e : client.of(EventType::Disconnect)) AVER_INFO("  diag  client disconnect reason {} at +{}", static_cast<int>(e.reason), e.at - 1000);
        }
        check(arrived, "a 1 MB packet and a 3000 byte packet arrive through 15% loss, duplication and reordering");
        const auto rx = server.of(EventType::Receive);
        check(rx.size() == 2 && rx[0].packet == big && rx[1].packet == small, "intact and in order");
    }
    {
        Rig r(23);
        r.net.config.minDelayMs = r.net.config.maxDelayMs = 5;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        if (!cp) { check(false, "connected"); return; }
        const auto mid = pattern(20000, 7);
        cp->send(0, mid.data(), mid.size(), 0);
        cp->send(1, mid.data(), mid.size(), kFlagUnsequenced);
        r.settle(1000);
        const auto rx = server.of(EventType::Receive);
        check(rx.size() == 2 && rx[0].packet == mid && rx[1].packet == mid, "unreliable ordered and unsequenced 20000 byte packets fragment and reassemble");
    }
    {
        Rig r(24);
        r.net.config.lossProbability = 0.3;
        r.net.config.minDelayMs = r.net.config.maxDelayMs = 5;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        if (!cp) { check(false, "connected"); return; }
        const auto mid = pattern(20000, 8);
        for (int i = 0; i < 40; ++i) { cp->send(0, mid.data(), mid.size(), 0); r.net.advance(5); r.pump(); }
        r.settle(1000);
        bool whole = true;
        for (const Rec& e : server.of(EventType::Receive)) whole &= e.packet == mid;
        check(whole, "an unreliable fragmented packet is delivered whole or not at all");
        check(server.count(EventType::Receive) < 40, "with 30% loss most of the 18-fragment packets do not survive");
    }
    {
        Rig r;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        if (!cp) { check(false, "connected"); return; }
        std::vector<u8> huge(1);
        check(cp->send(0, huge.data(), kMaxPacketSize + 1, kFlagReliable) == kSendTooBig, "a packet over the size cap is refused");
        check(cp->send(9, huge.data(), 1, kFlagReliable) == kSendBadChannel, "a channel past the negotiated count is refused");
        check(cp->send(0xFF, huge.data(), 1, kFlagReliable) == kSendBadChannel, "the control channel is not for packets");
    }
}

static void testChannels() {
    AVER_INFO("channels are independent");
    Rig r(31);
    r.net.config.minDelayMs = r.net.config.maxDelayMs = 50;
    Node& server = r.add(5000);
    Node& client = r.add(5001);
    Peer* cp = connectPair(r, client, server, 4);
    if (!cp) { check(false, "connected"); return; }
    check(cp->channelCount() == 4, "four channels");
    const auto big = pattern(1u << 20, 1);
    cp->send(0, big.data(), big.size(), kFlagReliable);
    auto small = u32pkt(0xBEEF);
    cp->send(1, small.data(), small.size(), kFlagReliable);
    r.run([&] { return server.count(EventType::Receive) >= 2; }, 120000);
    const auto rx = server.of(EventType::Receive);
    check(rx.size() == 2 && rx[0].channel == 1 && rx[1].channel == 0, "a small packet on channel 1 overtakes a 1 MB packet still arriving on channel 0");
    check(rx.size() == 2 && rx[0].at + 200 < rx[1].at, "and well before it finishes");

    // a lossy wire: per-channel order holds on four channels at once
    Rig r2(32);
    r2.net.config.lossProbability = 0.3;
    r2.net.config.minDelayMs = 5; r2.net.config.maxDelayMs = 70;
    Node& s2 = r2.add(5000);
    Node& c2 = r2.add(5001);
    Peer* cp2 = connectPair(r2, c2, s2, 4);
    if (!cp2) { check(false, "connected (lossy)"); return; }
    for (u32 i = 0; i < 400; ++i) { auto p = u32pkt(i); cp2->send(i % 4, p.data(), p.size(), kFlagReliable); }
    r2.run([&] { return s2.count(EventType::Receive) >= 400; }, 600000);
    u32 next[4] = {0, 1, 2, 3};
    bool ok = s2.count(EventType::Receive) == 400;
    for (const Rec& e : s2.of(EventType::Receive)) { if (e.channel > 3 || pktU32(e.packet) != next[e.channel]) { ok = false; break; } next[e.channel] += 4; }
    check(ok, "four channels each deliver in order through 30% loss");
}

static void testDisconnects() {
    AVER_INFO("disconnect and timeout events");
    {
        Rig r;
        r.net.config.minDelayMs = r.net.config.maxDelayMs = 15;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        if (!cp) { check(false, "connected"); return; }
        cp->disconnect(0xD15C0);
        check(r.run([&] { return server.count(EventType::Disconnect) && client.count(EventType::Disconnect); }, 20000), "a graceful disconnect gives both sides a Disconnect event");
        const Rec s = server.of(EventType::Disconnect)[0], c = client.of(EventType::Disconnect)[0];
        check(s.data == 0xD15C0 && s.reason == DisconnectReason::Remote, "the remote side sees the data word and reason Remote");
        check(c.reason == DisconnectReason::Local, "the requesting side sees reason Local");
        check(server.host->connectedCount() == 0 && client.host->connectedCount() == 0, "both slots are free");
        check(cp->state() == PeerState::Disconnected, "the local peer is Disconnected");
        // the slot can be used again
        Peer* again = client.host->connect(server.addr, 2, 5, r.net.now());
        check(again != nullptr && r.run([&] { return server.count(EventType::Connect) == 2 && client.count(EventType::Connect) == 2; }, 20000), "the same pair connects again");
    }
    {
        Rig r;
        r.net.config.minDelayMs = r.net.config.maxDelayMs = 15;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        if (!cp) { check(false, "connected"); return; }
        cp->disconnectNow(9);
        check(cp->state() == PeerState::Disconnected && client.count(EventType::Disconnect) == 0, "disconnectNow resets at once and raises no local event");
        check(r.run([&] { return server.count(EventType::Disconnect) == 1; }, 5000) && server.of(EventType::Disconnect)[0].data == 9, "the remote side is told");
    }
    {
        Rig r;
        r.net.config.minDelayMs = r.net.config.maxDelayMs = 15;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        if (!cp) { check(false, "connected"); return; }
        for (u32 i = 0; i < 50; ++i) { auto p = u32pkt(i); cp->send(0, p.data(), p.size(), kFlagReliable); }
        cp->disconnectLater(3);
        check(cp->state() == PeerState::DisconnectLater, "disconnectLater waits");
        r.run([&] { return server.count(EventType::Disconnect) == 1; }, 20000);
        check(server.count(EventType::Receive) == 50, "all queued data was delivered before the disconnect");
        const auto ev = server.log;
        size_t lastRx = 0, disc = 0;
        for (size_t i = 0; i < ev.size(); ++i) { if (ev[i].type == EventType::Receive) lastRx = i; if (ev[i].type == EventType::Disconnect) disc = i; }
        check(disc > lastRx, "and the Disconnect event came after the last packet");
    }
    {
        Rig r;
        r.net.config.minDelayMs = r.net.config.maxDelayMs = 15;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        if (!cp) { check(false, "connected"); return; }
        Peer* sp = serverPeer(server);
        cp->setTimeouts(4, 800, 3000);
        sp->setTimeouts(4, 800, 3000);
        const u64 t0 = r.net.now();
        r.net.blackhole(server.addr, true);
        check(r.run([&] { return client.count(EventType::Disconnect) == 1; }, 60000), "a peer that stops answering times out");
        const Rec c = client.of(EventType::Disconnect)[0];
        check(c.reason == DisconnectReason::Timeout, "reason Timeout");
        check(c.at - t0 >= 800 && c.at - t0 <= 6000, "after about the configured time (min 800 ms, max 3000 ms)");
        check(r.run([&] { return server.count(EventType::Disconnect) == 1; }, 60000) && server.of(EventType::Disconnect)[0].reason == DisconnectReason::Timeout, "the silent side times out too");
    }
    {
        Rig r;
        Node& client = r.add(5001);
        Peer* cp = client.host->connect(Address::loopback4(5999), 2, 0, r.net.now());       // nobody there
        cp->setTimeouts(4, 600, 2000);
        check(r.run([&] { return client.count(EventType::Disconnect) == 1; }, 30000), "connecting to nobody fails");
        check(client.of(EventType::Disconnect)[0].reason == DisconnectReason::ConnectFailed && client.count(EventType::Connect) == 0, "reason ConnectFailed, never a Connect");
    }
    {
        Rig r;
        HostConfig sc; sc.maxPeers = 2;
        Node& server = r.add(5000, sc);
        Node& c1 = r.add(5001);
        Node& c2 = r.add(5002);
        Node& c3 = r.add(5003);
        c1.host->connect(server.addr, 2, 1, r.net.now());
        c2.host->connect(server.addr, 2, 2, r.net.now());
        r.run([&] { return server.count(EventType::Connect) == 2; }, 10000);
        Peer* p3 = c3.host->connect(server.addr, 2, 3, r.net.now());
        p3->setTimeouts(4, 600, 2000);
        r.run([&] { return c3.count(EventType::Disconnect) == 1; }, 30000);
        check(server.count(EventType::Connect) == 2 && c3.count(EventType::Connect) == 0 && c3.count(EventType::Disconnect) == 1, "peer limit: a third client is not accepted");
        HostConfig rc;
        Node& s2 = r.add(5010, rc);
        s2.host->setRefuseConnections(true);
        Node& c4 = r.add(5004);
        Peer* p4 = c4.host->connect(s2.addr, 2, 0, r.net.now());
        p4->setTimeouts(4, 600, 2000);
        r.run([&] { return c4.count(EventType::Disconnect) == 1; }, 30000);
        check(s2.count(EventType::Connect) == 0 && c4.count(EventType::Connect) == 0, "refuseConnections turns every client away");
    }
}

static void testPingAndRtt() {
    AVER_INFO("ping, RTT and jitter");
    Rig r(41);
    r.net.config.minDelayMs = r.net.config.maxDelayMs = 40;
    Node& server = r.add(5000);
    Node& client = r.add(5001);
    Peer* cp = connectPair(r, client, server);
    if (!cp) { check(false, "connected"); return; }
    r.settle(20000);                       // idle: only pings keep it alive
    check(client.count(EventType::Disconnect) == 0 && server.count(EventType::Disconnect) == 0, "an idle connection stays up for 20 s on pings alone");
    check(cp->roundTripTime() >= 78 && cp->roundTripTime() <= 90, "the RTT estimate is the 80 ms the wire gives (got " + std::to_string(cp->roundTripTime()) + ")");
    check(cp->roundTripTimeVariance() <= 6, "no jitter on a steady wire");
    check(cp->lowestRoundTripTime() >= 78, "lowest RTT");

    Rig j(42);
    j.net.config.minDelayMs = 10; j.net.config.maxDelayMs = 110;
    Node& s2 = j.add(5000);
    Node& c2 = j.add(5001);
    Peer* cp2 = connectPair(j, c2, s2);
    if (!cp2) { check(false, "connected"); return; }
    cp2->setPingInterval(100);
    check(cp2->pingInterval() == 100, "ping interval is settable");
    j.settle(20000);
    check(cp2->roundTripTimeVariance() > 15, "a wide delay range shows up as RTT variance (got " + std::to_string(cp2->roundTripTimeVariance()) + ")");
    check(cp2->roundTripTime() > 60 && cp2->roundTripTime() < 200, "RTT near twice the mean delay");
}

static void testBandwidth() {
    AVER_INFO("bandwidth limits");
    auto transfer = [](u32 outBw, u32 clientInBw) {
        Rig r(51);
        r.net.config.minDelayMs = r.net.config.maxDelayMs = 5;
        HostConfig sc; sc.outBandwidth = outBw;
        HostConfig cc; cc.inBandwidth = clientInBw;
        Node& server = r.add(5000, sc);
        Node& client = r.add(5001, cc);
        Peer* cp = connectPair(r, client, server);
        Peer* sp = cp ? serverPeer(server) : nullptr;
        if (!sp) return u64(0);
        r.settle(100);
        const auto blob = pattern(1000, 3);
        for (int i = 0; i < 100; ++i) sp->send(0, blob.data(), blob.size(), kFlagReliable);
        const u64 t0 = r.net.now();
        const bool ok = r.run([&] { return client.count(EventType::Receive) >= 100; }, 120000);
        return ok ? r.net.now() - t0 : u64(0);
    };
    const u64 free = transfer(0, 0);
    const u64 capped = transfer(50000, 0);
    const u64 advertised = transfer(0, 50000);
    check(free > 0 && free < 600, "unlimited: 100 KB moves in well under a second (" + std::to_string(free) + " ms)");
    check(capped >= 1500 && capped <= 3500, "outBandwidth 50 KB/s: 100 KB takes about two seconds (" + std::to_string(capped) + " ms)");
    check(advertised >= 1500 && advertised <= 3500, "the client's advertised inBandwidth 50 KB/s limits the server (" + std::to_string(advertised) + " ms)");

    // unreliable traffic over the limit is dropped, not queued without end; reliable traffic is not
    Rig r(52);
    r.net.config.minDelayMs = r.net.config.maxDelayMs = 5;
    HostConfig sc; sc.outBandwidth = 20000;
    Node& server = r.add(5000, sc);
    Node& client = r.add(5001);
    Peer* cp = connectPair(r, client, server);
    Peer* sp = cp ? serverPeer(server) : nullptr;
    if (!sp) { check(false, "connected"); return; }
    r.settle(100);
    const auto blob = pattern(500, 2);
    for (int i = 0; i < 2000; ++i) sp->send(0, blob.data(), blob.size(), 0);
    auto rel = u32pkt(0x600D);
    sp->send(1, rel.data(), rel.size(), kFlagReliable);
    r.settle(5000);
    check(sp->packetsDropped() > 1000, "an unreliable flood over the budget is dropped at the sender (" + std::to_string(sp->packetsDropped()) + ")");
    size_t unrel = 0, relCount = 0;
    for (const Rec& e : client.of(EventType::Receive)) (e.channel == 0 ? unrel : relCount)++;
    check(unrel > 0 && unrel < 1000, "and only what the budget allows arrives (" + std::to_string(unrel) + ")");
    check(relCount == 1, "the reliable packet still arrives");
    // changing the limit later reaches the peer
    Rig q(53);
    q.net.config.minDelayMs = q.net.config.maxDelayMs = 5;
    Node& s3 = q.add(5000);
    Node& c3 = q.add(5001);
    Peer* cp3 = connectPair(q, c3, s3);
    Peer* sp3 = cp3 ? serverPeer(s3) : nullptr;
    if (!sp3) { check(false, "connected"); return; }
    c3.host->setBandwidthLimit(40000, 0);
    q.settle(300);
    const auto b3 = pattern(1000, 3);
    for (int i = 0; i < 80; ++i) sp3->send(0, b3.data(), b3.size(), kFlagReliable);
    const u64 t0 = q.net.now();
    q.run([&] { return c3.count(EventType::Receive) >= 80; }, 60000);
    const u64 took = q.net.now() - t0;
    check(took >= 1400 && took <= 3000, "setBandwidthLimit after connecting is advertised to the peer (" + std::to_string(took) + " ms)");
}

static void testBroadcastAndMany() {
    AVER_INFO("a server with several clients");
    Rig r(61);
    r.net.config.lossProbability = 0.1;
    r.net.config.minDelayMs = 5; r.net.config.maxDelayMs = 40;
    Node& server = r.add(5000);
    std::vector<Node*> clients;
    for (int i = 0; i < 5; ++i) {
        Node& c = r.add(static_cast<u16>(5001 + i));
        c.host->connect(server.addr, 2, 100 + i, r.net.now());
        clients.push_back(&c);
    }
    check(r.run([&] { return server.count(EventType::Connect) == 5; }, 30000), "five clients connect");
    std::vector<u32> data;
    for (const Rec& e : server.of(EventType::Connect)) data.push_back(e.data);
    std::sort(data.begin(), data.end());
    check(data == std::vector<u32>({100, 101, 102, 103, 104}), "each connect-data word arrives with its own peer");
    auto msg = u32pkt(0xB10C);
    server.host->broadcast(0, msg.data(), msg.size(), kFlagReliable);
    check(r.run([&] { for (Node* c : clients) if (c->count(EventType::Receive) < 1) return false; return true; }, 30000), "a broadcast reaches every client");
    // a client restarting from the same address replaces its old peer
    Node& again = *clients[0];
    again.host.reset();
    again.host = std::make_unique<Host>(r.net.bind(again.addr), HostConfig{});
    again.log.clear();
    again.host->connect(server.addr, 2, 555, r.net.now());
    check(r.run([&] { return again.count(EventType::Connect) == 1; }, 30000), "a restarted client connects again");
    check(server.count(EventType::Disconnect) >= 1, "and the server drops the stale peer of that address");
}

static void testHostile() {
    AVER_INFO("hostile datagrams");
    Rig r(71);
    Node& server = r.add(5000);
    Node& client = r.add(5001);
    Peer* cp = connectPair(r, client, server);
    if (!cp) { check(false, "connected"); return; }
    auto attacker = r.net.bind(Address::loopback4(6000));
    u64 rng = 99;
    auto rnd = [&] { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(rng >> 33); };
    const Address target = server.addr;
    for (int i = 0; i < 4000; ++i) {                      // pure noise: the checksum rejects it
        std::vector<u8> d(rnd() % 1300);
        for (u8& b : d) b = static_cast<u8>(rnd());
        attacker->send(target, d.data(), d.size());
    }
    r.settle(50);
    check(server.host->stats().badChecksum + server.host->stats().malformed > 3000, "random datagrams are rejected by the checksum or the length check");
    for (int i = 0; i < 40000; ++i) {                     // noise with a valid checksum: the parser must survive it
        std::vector<u8> d(12 + rnd() % 200);
        for (u8& b : d) b = static_cast<u8>(rnd());
        d[4] = static_cast<u8>(rnd() % 3 == 0 ? 0xFF : 0); d[5] = d[4] ? 0xFF : 0;      // peer id: none, or slot 0
        d[10] = static_cast<u8>(1 + rnd() % 4);
        if (i % 2 == 0 && d.size() > 18) { d[12] = static_cast<u8>(2 + rnd() % 10); d[13] = static_cast<u8>(rnd() % 3); }
        std::memset(d.data(), 0, 4);
        const u32 c = crc32(d.data(), d.size());
        for (int k = 0; k < 4; ++k) d[k] = static_cast<u8>(c >> (8 * k));
        attacker->send(target, d.data(), d.size());
        if (i % 500 == 0) r.pump();
    }
    r.settle(200);
    check(true, "40000 checksum-valid random datagrams did not crash the host");
    check(server.host->connectedCount() <= 1, "and no phantom peers appeared");
    // the real connection still works afterwards
    auto p = u32pkt(0x7357);
    Peer* sp = serverPeer(server);
    const size_t before = client.count(EventType::Receive);
    if (sp && sp->state() == PeerState::Connected) sp->send(0, p.data(), p.size(), kFlagReliable);
    r.run([&] { return client.count(EventType::Receive) > before; }, 20000);
    check(client.count(EventType::Receive) > before, "the genuine connection still carries traffic");

    // a datagram from the wrong address for a live peer id is ignored
    HostConfig nc; nc.checksum = false;
    Rig r2(72);
    Node& s2 = r2.add(5000, nc);
    Node& c2 = r2.add(5001, nc);
    Peer* cp2 = connectPair(r2, c2, s2);
    check(cp2 != nullptr, "hosts that agree to skip the checksum still talk");
}

static void testDeterminism() {
    AVER_INFO("determinism");
    auto run = [] {
        Rig r(99);
        r.net.config.lossProbability = 0.3;
        r.net.config.duplicateProbability = 0.2;
        r.net.config.minDelayMs = 5; r.net.config.maxDelayMs = 90;
        Node& server = r.add(5000);
        Node& client = r.add(5001);
        Peer* cp = connectPair(r, client, server);
        if (!cp) return std::vector<u64>{0};
        for (u32 i = 0; i < 100; ++i) { auto p = u32pkt(i); cp->send(0, p.data(), p.size(), i % 3 == 0 ? kFlagReliable : 0); }
        r.settle(5000);
        return std::vector<u64>{r.net.stats.sent, r.net.stats.dropped, r.net.stats.duplicated, server.count(EventType::Receive), cp->packetsLost(), r.net.now()};
    };
    check(run() == run(), "the same seed gives the same run, down to the datagram counts");
}

int main() {
    AVER_INFO("NetProtocolTest");
    testCrc();
    testHandshake();
    testHandshakeUnderLoss();
    testReliableOrder();
    testReliableBothWays();
    testUnreliableOrdered();
    testUnsequenced();
    testFragmentation();
    testChannels();
    testDisconnects();
    testPingAndRtt();
    testBandwidth();
    testBroadcastAndMany();
    testHostile();
    testDeterminism();
    if (g_failures) { AVER_ERROR("{} check(s) failed", g_failures); return 1; }
    AVER_INFO("all checks passed");
    return 0;
}
