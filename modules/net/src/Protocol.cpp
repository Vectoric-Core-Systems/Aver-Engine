#include "aver/net/Protocol.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>

namespace aver::net {

namespace {

constexpr u32 kHeaderSize = 12;      // checksum u32, peer id u16, session u16, sent time u16, command count u8, flags u8
constexpr u32 kCmdHeader = 6;        // type u8, channel u8, sequence u16, payload length u16
constexpr u16 kNoPeerId = 0xFFFF;
constexpr u8 kControl = 0xFF;        // the channel of the handshake, pings and disconnects
constexpr u16 kProtoVersion = 1;
constexpr u32 kSendWindow = 256;     // reliable commands in flight per channel
constexpr u32 kRecvWindow = 512;     // how far ahead of the next expected sequence a reliable command is buffered
constexpr u32 kUnseqWindow = 1024;
constexpr u32 kMinRto = 30, kInitialRto = 500;
constexpr usize kMaxAssemblies = 16;
constexpr u32 kMaxDatagram = 4096;
constexpr u32 kMinMtu = 576;
constexpr u64 kFragMinChunk = kMinMtu - kHeaderSize - kCmdHeader - 20;        // the data in a fragment of a sender on the smallest MTU
constexpr u64 kFragMaxChunk = kMaxDatagram - kHeaderSize - kCmdHeader - 20;   // and on the largest

enum Cmd : u8 {
    kAck = 1, kConnect = 2, kVerify = 3, kDisconnect = 4, kPing = 5, kSendReliable = 6, kSendUnreliable = 7, kSendUnsequenced = 8,
    kSendFragment = 9, kSendUnrelFragment = 10, kBandwidth = 11,
};
constexpr u8 kAckBit = 0x80;         // the command is reliable: acknowledge it
constexpr u8 kUnseqBit = 0x40;       // an unreliable fragment that is unsequenced
constexpr u8 kTypeMask = 0x3F;

inline void p8(std::vector<u8>& b, u32 v) { b.push_back(static_cast<u8>(v)); }
inline void p16(std::vector<u8>& b, u32 v) { b.push_back(static_cast<u8>(v)); b.push_back(static_cast<u8>(v >> 8)); }
inline void p32(std::vector<u8>& b, u32 v) { p16(b, v & 0xFFFF); p16(b, v >> 16); }

struct Rd {
    const u8* p; usize n; usize i = 0; bool bad = false;
    Rd(const u8* d, usize len) : p(d), n(len) {}
    u8 g8() { if (i + 1 > n) { bad = true; return 0; } return p[i++]; }
    u16 g16() { const u16 lo = g8(); const u16 hi = g8(); return static_cast<u16>(lo | (hi << 8)); }
    u32 g32() { const u32 lo = g16(); const u32 hi = g16(); return lo | (hi << 16); }
    const u8* take(usize k) { if (i + k > n) { bad = true; return nullptr; } const u8* r = p + i; i += k; return r; }
    usize left() const { return n - i; }
};

u32 crcStep(u32 c, const u8* d, usize n) {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) { u32 x = i; for (int k = 0; k < 8; ++k) x = (x & 1) ? (0xEDB88320u ^ (x >> 1)) : (x >> 1); t[i] = x; }
        return t;
    }();
    for (usize i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}

inline u16 t16(u64 now) { return static_cast<u16>(now & 0xFFFF); }
inline bool seqNewer(u16 a, u16 b) { return static_cast<i16>(static_cast<u16>(a - b)) > 0; }   // a is after b

// A fragment whose count, total and offset no sender could have produced is refused, so a few bytes of wire cannot claim a huge packet.
inline bool fragmentOk(u32 count, u32 num, u32 total, u32 off, usize dl) {
    if (count == 0 || num >= count || total > kMaxPacketSize || off > total || dl > total - off) return false;
    if (total > static_cast<u64>(count) * kFragMaxChunk || static_cast<u64>(count - 1) * kFragMinChunk >= total) return false;
    return off <= static_cast<u64>(num) * kFragMaxChunk && off >= static_cast<u64>(num) * kFragMinChunk;
}

} // namespace

u32 crc32(const u8* data, usize len) { return ~crcStep(0xFFFFFFFFu, data, len); }

// ------------------------------------------------------------------ peer state

namespace {

struct OutCmd {
    u8 type = 0, channel = 0;
    u16 seq = 0;
    std::vector<u8> payload;
    u64 sentAt = 0, firstSent = 0;
    u32 timeout = 0, timeoutLimit = 0;
    bool resendDue = false;
    usize wireSize() const { return kCmdHeader + payload.size(); }
    bool reliable() const { return (type & kAckBit) != 0; }
};

struct InCmd { u8 type = 0; std::vector<u8> payload; };

struct Assembly {
    std::vector<u8> data;         // grows with the fragments that arrive, never ahead of them
    std::vector<bool> got;        // unreliable fragments: which have arrived
    u32 remaining = 0, total = 0, received = 0;
    u16 group = 0;
    bool unseq = false;
};

struct Channel {
    u16 outReliable = 0, outUnreliable = 0, inReliable = 0, inUnreliable = 0;
    u32 inFlight = 0;
    std::map<u16, InCmd> pending;             // reliable commands that arrived ahead of order
    std::map<u16, Assembly> relAsm;           // reliable fragments being put together, by first sequence
    std::deque<Assembly> unrelAsm;            // unreliable fragments, oldest first
};

struct AckEntry { u8 channel; u16 seq; u16 sent; };

} // namespace

struct Peer::Impl {
    Host::Impl* host = nullptr;
    u16 index = 0;
    u32 generation = 0;
    PeerState state = PeerState::Disconnected;
    Address address;
    u16 remoteIndex = kNoPeerId;
    u16 session = 0;
    u32 connectId = 0;
    u32 connectData = 0;
    u32 requestedChannels = 0;
    u32 mtu = 1200;
    std::vector<Channel> channels;
    Channel control;
    std::deque<OutCmd> outgoing;
    std::deque<OutCmd> sentReliable;
    std::deque<AckEntry> acks;
    u64 unrelQueued = 0;
    u16 outUnseqGroup = 0;
    u16 unseqHighest = 0;
    std::bitset<kUnseqWindow> unseqSeen;
    // timing
    u32 rtt = 0, rttVar = 0, lastRtt = 0, lowestRtt = 0;
    bool rttSet = false;
    u32 pingInterval = 500, timeoutLimit = 32, timeoutMin = 5000, timeoutMax = 30000;
    u32 throttleInterval = 5000, throttleAccel = 2, throttleDecel = 2;
    u64 lastSend = 0, lastReceive = 0, connectStart = 0, connectAckedAt = 0;
    // bandwidth
    u32 remoteIn = 0, remoteOut = 0;
    double tokens = 0;
    u64 tokenAt = 0;
    // statistics
    u64 packetsSent = 0, packetsLost = 0, packetsDropped = 0, bytesSent = 0, bytesReceived = 0;
    u64 lossEpoch = 0, epochSent = 0, epochLost = 0;
    u32 loss = 0, lossVar = 0;
    // disconnect
    u32 laterData = 0;
    bool zombie = false;

    void clear() {
        state = PeerState::Disconnected;
        address = Address{};
        remoteIndex = kNoPeerId; session = 0; connectId = 0; connectData = 0; requestedChannels = 0;
        channels.clear(); control = Channel{};
        outgoing.clear(); sentReliable.clear(); acks.clear();
        unrelQueued = 0; outUnseqGroup = 0; unseqHighest = 0; unseqSeen.reset();
        rtt = rttVar = lastRtt = lowestRtt = 0; rttSet = false;
        pingInterval = 500; timeoutLimit = 32; timeoutMin = 5000; timeoutMax = 30000;
        throttleInterval = 5000; throttleAccel = 2; throttleDecel = 2;
        lastSend = lastReceive = connectStart = connectAckedAt = 0;
        remoteIn = remoteOut = 0; tokens = 0; tokenAt = 0;
        packetsSent = packetsLost = packetsDropped = bytesSent = bytesReceived = 0;
        lossEpoch = epochSent = epochLost = 0; loss = lossVar = 0;
        laterData = 0; zombie = false;
    }
    Channel* chan(u8 idx) {
        if (idx == kControl) return &control;
        return idx < channels.size() ? &channels[idx] : nullptr;
    }
    u32 rto() const { return rttSet ? std::max<u32>(kMinRto, rtt + 4 * rttVar) : kInitialRto; }
    bool live() const { return state != PeerState::Disconnected; }
};

// ------------------------------------------------------------------ host

struct Host::Impl {
    HostConfig cfg;
    std::unique_ptr<DatagramSocket> sock;
    std::vector<std::unique_ptr<Peer>> peers;
    std::deque<Event> events;
    HostStats stats;
    u64 now = 0;
    u64 rng = 0;
    double tokens = 0;
    u64 tokenAt = 0;
    std::vector<u8> tx, rx;

    using P = Peer::Impl;

    u64 rnd() {
        rng += 0x9E3779B97F4A7C15ull;
        u64 z = rng;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    u32 channelCap() const { return cfg.channelLimit ? std::min(cfg.channelLimit, kMaxChannelCount) : kMaxChannelCount; }

    P* slot(u32 i) { return i < peers.size() && peers[i] ? peers[i]->d_.get() : nullptr; }

    Peer* alloc(const Address& a) {
        for (u32 i = 0; i < peers.size(); ++i) {
            if (!peers[i]) {
                peers[i].reset(new Peer());
                peers[i]->d_->host = this;
                peers[i]->d_->index = static_cast<u16>(i);
            }
            P& p = *peers[i]->d_;
            if (p.live()) continue;
            const u32 gen = p.generation + 1;
            p.clear();
            p.generation = gen;
            p.address = a;
            p.mtu = cfg.mtu;
            p.lastReceive = p.lastSend = p.connectStart = now;
            p.tokenAt = now;
            p.lossEpoch = now;
            return peers[i].get();
        }
        return nullptr;
    }

    // ---- events and teardown

    void emit(EventType t, P& p, u8 ch, u32 data, DisconnectReason why, std::vector<u8>&& packet = {}) {
        Event e;
        e.type = t; e.peer = peers[p.index].get(); e.peerIndex = p.index; e.peerGeneration = p.generation;
        e.channel = ch; e.data = data; e.reason = why; e.packet = std::move(packet);
        events.push_back(std::move(e));
    }
    void drop(P& p) {                       // the slot is free at once; the generation moves so old references stop matching
        const u32 gen = p.generation + 1;
        p.clear();
        p.generation = gen;
    }
    void finish(P& p, DisconnectReason why, u32 data, bool announce) {
        if (announce) emit(EventType::Disconnect, p, 0, data, why);
        drop(p);
    }
    void zombify(P& p, DisconnectReason why, u32 data) {   // the acknowledgement must still go out, so the reset waits for the flush
        emit(EventType::Disconnect, p, 0, data, why);
        p.outgoing.clear(); p.sentReliable.clear(); p.unrelQueued = 0;
        p.state = PeerState::Zombie;
        p.zombie = true;
    }
    void timedOut(P& p) {
        const bool pre = p.state == PeerState::Connecting || p.state == PeerState::AcknowledgingConnect;
        finish(p, pre ? DisconnectReason::ConnectFailed : DisconnectReason::Timeout, 0, p.state != PeerState::AcknowledgingConnect);
    }

    // ---- queueing

    OutCmd& queueReliable(P& p, u8 type, u8 chIdx, std::vector<u8>&& payload) {
        Channel* ch = p.chan(chIdx);
        OutCmd c;
        c.type = static_cast<u8>(type | kAckBit); c.channel = chIdx; c.seq = ++ch->outReliable; c.payload = std::move(payload);
        p.outgoing.push_back(std::move(c));
        return p.outgoing.back();
    }
    void queueAck(P& p, u8 ch, u16 seq, u16 sent) { p.acks.push_back(AckEntry{ch, seq, sent}); }

    u32 effectiveRate(const P& p) const {
        u32 r = cfg.outBandwidth;
        if (p.remoteIn && (!r || p.remoteIn < r)) r = p.remoteIn;
        return r;
    }

    int sendPacket(P& p, u8 chIdx, const u8* data, usize len, u32 flags) {
        if (p.state != PeerState::Connected) return kSendBadState;
        Channel* ch = chan(p, chIdx);
        if (!ch || chIdx == kControl) return kSendBadChannel;
        if (len > kMaxPacketSize) return kSendTooBig;
        const bool reliable = (flags & kFlagReliable) != 0;
        const bool unseq = !reliable && (flags & kFlagUnsequenced) != 0;
        if (!reliable) {
            const u32 rate = effectiveRate(p);
            if (rate && p.unrelQueued > rate / 4 + 4ull * p.mtu) { ++p.packetsDropped; return kSendOk; }
        }
        const usize single = p.mtu - kHeaderSize - kCmdHeader - (reliable ? 0 : 2);
        if (len <= single) {
            std::vector<u8> pl;
            pl.reserve(len + 2);
            if (reliable) {
                pl.assign(data, data + len);
                queueReliable(p, kSendReliable, chIdx, std::move(pl));
            } else {
                p16(pl, unseq ? ++p.outUnseqGroup : ++ch->outUnreliable);
                pl.insert(pl.end(), data, data + len);
                OutCmd c;
                c.type = unseq ? kSendUnsequenced : kSendUnreliable; c.channel = chIdx; c.payload = std::move(pl);
                p.unrelQueued += c.wireSize();
                p.outgoing.push_back(std::move(c));
            }
            return kSendOk;
        }
        const usize chunk = p.mtu - kHeaderSize - kCmdHeader - 20;
        const u32 count = static_cast<u32>((len + chunk - 1) / chunk);
        const u16 start = static_cast<u16>(ch->outReliable + 1);
        const u16 group = reliable ? start : (unseq ? ++p.outUnseqGroup : ++ch->outUnreliable);
        for (u32 n = 0; n < count; ++n) {
            const usize off = static_cast<usize>(n) * chunk;
            const usize take = std::min(chunk, len - off);
            std::vector<u8> pl;
            pl.reserve(take + 20);
            if (reliable) {
                p16(pl, start); p32(pl, count); p32(pl, n); p32(pl, static_cast<u32>(len)); p32(pl, static_cast<u32>(off));
                pl.insert(pl.end(), data + off, data + off + take);
                queueReliable(p, kSendFragment, chIdx, std::move(pl));
            } else {
                p16(pl, group); p16(pl, count); p16(pl, n); p32(pl, static_cast<u32>(len)); p32(pl, static_cast<u32>(off));
                pl.insert(pl.end(), data + off, data + off + take);
                OutCmd c;
                c.type = static_cast<u8>(kSendUnrelFragment | (unseq ? kUnseqBit : 0)); c.channel = chIdx; c.payload = std::move(pl);
                p.unrelQueued += c.wireSize();
                p.outgoing.push_back(std::move(c));
            }
        }
        return kSendOk;
    }
    Channel* chan(P& p, u8 i) { return p.chan(i); }

    // ---- sending

    void refill(double& bucket, u64& at, u32 rate) {
        if (!rate) return;
        const double burst = std::max<double>(rate / 10.0, 2.0 * cfg.mtu);
        if (now > at) bucket = std::min(burst, bucket + rate * static_cast<double>(now - at) / 1000.0);
        at = now;
    }
    bool budget(P& p) {
        if (cfg.outBandwidth) { refill(tokens, tokenAt, cfg.outBandwidth); if (tokens <= 0) return false; }
        if (p.remoteIn) { refill(p.tokens, p.tokenAt, p.remoteIn); if (p.tokens <= 0) return false; }
        return true;
    }

    static void append(std::vector<u8>& d, u8 type, u8 ch, u16 seq, const std::vector<u8>& payload) {
        p8(d, type); p8(d, ch); p16(d, seq); p16(d, static_cast<u32>(payload.size()));
        d.insert(d.end(), payload.begin(), payload.end());
    }

    void sendDatagram(P& p, u32 count) {
        const u16 peerId = p.state == PeerState::Connecting ? kNoPeerId : p.remoteIndex;
        std::memset(tx.data(), 0, 4);
        tx[4] = static_cast<u8>(peerId); tx[5] = static_cast<u8>(peerId >> 8);
        tx[6] = static_cast<u8>(p.session); tx[7] = static_cast<u8>(p.session >> 8);
        tx[8] = static_cast<u8>(t16(now)); tx[9] = static_cast<u8>(t16(now) >> 8);
        tx[10] = static_cast<u8>(count); tx[11] = 0;
        if (cfg.checksum) {
            const u32 c = crc32(tx.data(), tx.size());
            for (int k = 0; k < 4; ++k) tx[k] = static_cast<u8>(c >> (8 * k));
        }
        if (sock) sock->send(p.address, tx.data(), tx.size());
        const double n = static_cast<double>(tx.size());
        if (cfg.outBandwidth) tokens -= n;
        if (p.remoteIn) p.tokens -= n;
        p.lastSend = now;
        p.bytesSent += tx.size();
        ++stats.sentPackets;
        stats.sentBytes += tx.size();
    }

    void flushPeer(P& p) {
        if (!sock || !p.live()) return;
        for (int guard = 0; guard < 8192; ++guard) {
            const bool ok = budget(p);
            if (!ok && p.acks.empty()) break;
            tx.assign(kHeaderSize, 0);
            u32 count = 0;
            const usize mtu = p.mtu;
            auto fits = [&](usize sz) { return count < 255 && tx.size() + sz <= mtu; };
            while (!p.acks.empty() && fits(kCmdHeader + 2)) {
                const AckEntry a = p.acks.front();
                p.acks.pop_front();
                p8(tx, kAck); p8(tx, a.channel); p16(tx, a.seq); p16(tx, 2); p16(tx, a.sent);
                ++count;
            }
            if (ok) {
                for (OutCmd& c : p.sentReliable) {
                    if (!c.resendDue || !fits(c.wireSize())) continue;
                    append(tx, c.type, c.channel, c.seq, c.payload);
                    c.resendDue = false; c.sentAt = now;
                    ++count; ++p.packetsSent; ++p.epochSent;
                }
                for (auto it = p.outgoing.begin(); it != p.outgoing.end();) {
                    OutCmd& c = *it;
                    Channel* ch = p.chan(c.channel);
                    if (c.reliable() && ch && ch->inFlight >= kSendWindow) { ++it; continue; }
                    if (!fits(c.wireSize())) {
                        if (count == 0 && kHeaderSize + c.wireSize() > mtu) { it = p.outgoing.erase(it); continue; }   // cannot ever fit
                        break;
                    }
                    append(tx, c.type, c.channel, c.seq, c.payload);
                    ++count; ++p.packetsSent;
                    if (c.reliable()) {
                        ++p.epochSent;
                        const u32 r = p.rto();
                        c.sentAt = now; c.firstSent = now; c.timeout = r; c.timeoutLimit = p.timeoutLimit * r;
                        if (ch) ++ch->inFlight;
                        p.sentReliable.push_back(std::move(c));
                    } else {
                        p.unrelQueued -= std::min<u64>(p.unrelQueued, c.wireSize());
                    }
                    it = p.outgoing.erase(it);
                }
            }
            if (count == 0) break;
            sendDatagram(p, count);
        }
    }

    // ---- timers

    void updateRtt(P& p, u32 sample) {
        sample = std::max<u32>(sample, 1);
        p.lastRtt = sample;
        if (!p.rttSet) { p.rtt = sample; p.rttVar = sample / 2; p.lowestRtt = sample; p.rttSet = true; return; }
        const u32 diff = sample > p.rtt ? sample - p.rtt : p.rtt - sample;
        p.rttVar = (3 * p.rttVar + diff) / 4;
        p.rtt = (7 * p.rtt + sample) / 8;
        p.lowestRtt = std::min(p.lowestRtt, sample);
    }

    void startDisconnect(P& p, u32 data) {
        p.outgoing.clear(); p.sentReliable.clear(); p.unrelQueued = 0;
        for (Channel& c : p.channels) c.inFlight = 0;
        p.control.inFlight = 0;
        p.state = PeerState::Disconnecting;
        std::vector<u8> pl;
        p32(pl, data);
        queueReliable(p, kDisconnect, kControl, std::move(pl));
    }

    void timers(P& p) {
        if (!p.live()) return;
        if (p.state == PeerState::Zombie) return;
        // resend or give up
        for (OutCmd& c : p.sentReliable) {
            if (now - c.sentAt < c.timeout) continue;
            const u64 elapsed = now - c.firstSent;
            if (elapsed >= p.timeoutMax || (c.timeout >= c.timeoutLimit && elapsed >= p.timeoutMin)) { timedOut(p); return; }
            c.timeout *= 2;
            c.resendDue = true;
            ++p.packetsLost; ++p.epochLost;
        }
        if (p.state == PeerState::Connecting && p.connectAckedAt && now - p.connectAckedAt >= p.timeoutMin) { timedOut(p); return; }
        // Silence from the other end, not our own sending, is what a ping tests: unreliable traffic out proves nothing about the wire.
        if (p.state == PeerState::Connected && p.sentReliable.empty() && now - p.lastReceive >= p.pingInterval &&
            std::none_of(p.outgoing.begin(), p.outgoing.end(), [](const OutCmd& c) { return c.reliable(); })) queueReliable(p, kPing, kControl, {});
        if (p.state == PeerState::DisconnectLater && p.outgoing.empty() && p.sentReliable.empty()) startDisconnect(p, p.laterData);
        if (now - p.lossEpoch >= 1000) {
            if (p.epochSent) {
                const u32 sample = static_cast<u32>(std::min<u64>(p.epochLost, p.epochSent) * kPacketLossScale / p.epochSent);
                const u32 diff = sample > p.loss ? sample - p.loss : p.loss - sample;
                p.lossVar = (3 * p.lossVar + diff) / 4;
                p.loss = (7 * p.loss + sample) / 8;
            }
            p.lossEpoch = now; p.epochSent = p.epochLost = 0;
        }
    }

    // ---- receiving

    void receiveAll() {
        if (!sock) return;
        rx.resize(65536);
        for (int i = 0; i < 1024; ++i) {
            Address from;
            const int n = sock->recv(from, rx.data(), rx.size());
            if (n == kWouldBlock) break;
            if (n < 0) break;
            handleDatagram(from, rx.data(), static_cast<usize>(n));
        }
    }

    void handleDatagram(const Address& from, const u8* d, usize n) {
        if (n < kHeaderSize || n > kMaxDatagram) { ++stats.malformed; return; }
        if (cfg.checksum) {
            const u32 want = static_cast<u32>(d[0]) | (static_cast<u32>(d[1]) << 8) | (static_cast<u32>(d[2]) << 16) | (static_cast<u32>(d[3]) << 24);
            const u8 zero[4] = {0, 0, 0, 0};
            const u32 got = ~crcStep(crcStep(0xFFFFFFFFu, zero, 4), d + 4, n - 4);
            if (want != got) { ++stats.badChecksum; return; }
        }
        Rd r(d + 4, n - 4);
        const u16 peerId = r.g16();
        const u16 session = r.g16();
        const u16 sent = r.g16();
        const u32 count = r.g8();
        r.g8();
        ++stats.receivedPackets;
        stats.receivedBytes += n;
        P* p = nullptr;
        if (peerId != kNoPeerId) {
            p = slot(peerId);
            if (!p || !p->live() || p->address != from || p->session != session) return;
            p->lastReceive = now;
            p->bytesReceived += n;
        }
        for (u32 i = 0; i < count; ++i) {
            if (r.left() < kCmdHeader) { ++stats.malformed; return; }
            const u8 type = r.g8();
            const u8 ch = r.g8();
            const u16 seq = r.g16();
            const u16 len = r.g16();
            const u8* pl = r.take(len);
            if (!pl) { ++stats.malformed; return; }
            std::vector<u8> payload(pl, pl + len);
            if (!p) {
                if ((type & kTypeMask) == kConnect) handleConnect(from, session, type, ch, seq, payload, sent);
                continue;
            }
            if (!p->live()) return;                // an earlier command in this datagram ended the peer
            handleCommand(*p, type, ch, seq, std::move(payload), sent);
        }
    }

    static bool allowed(PeerState s, u8 cmd) {
        switch (s) {
            case PeerState::Connecting: return cmd == kVerify || cmd == kAck || cmd == kDisconnect;
            case PeerState::AcknowledgingConnect: return cmd == kAck;
            case PeerState::Connected: case PeerState::DisconnectLater: return true;
            case PeerState::Disconnecting: return cmd == kAck || cmd == kDisconnect;
            default: return false;
        }
    }

    void handleConnect(const Address& from, u16 session, u8 type, u8 chIdx, u16 seq, const std::vector<u8>& pl, u16 sent) {
        if (cfg.refuseConnections || chIdx != kControl || !(type & kAckBit)) return;
        Rd r(pl.data(), pl.size());
        const u16 clientIndex = r.g16();
        u32 channels = r.g8();
        u32 mtu = r.g16();
        const u32 connectId = r.g32();
        const u32 data = r.g32();
        const u32 inBw = r.g32();
        const u32 outBw = r.g32();
        const u16 version = r.g16();
        if (r.bad || version != kProtoVersion || channels == 0 || static_cast<u16>(connectId) != session) return;
        for (auto& up : peers) {
            if (!up || !up->d_->live() || up->d_->address != from) continue;
            P& old = *up->d_;
            if (old.connectId == connectId) { queueAck(old, kControl, seq, sent); return; }   // the CONNECT again
            // The same address with a new connection id: the client restarted.
            if (old.state == PeerState::Connected || old.state == PeerState::DisconnectLater || old.state == PeerState::Disconnecting) finish(old, DisconnectReason::Timeout, 0, true);
            else finish(old, DisconnectReason::Timeout, 0, false);
        }
        Peer* pr = alloc(from);
        if (!pr) return;
        P& p = *pr->d_;
        channels = std::min(channels, channelCap());
        mtu = std::clamp<u32>(mtu, 576, kMaxDatagram);
        p.state = PeerState::AcknowledgingConnect;
        p.remoteIndex = clientIndex; p.session = session; p.connectId = connectId; p.connectData = data;
        p.channels.assign(channels, Channel{});
        p.mtu = std::min(mtu, cfg.mtu);
        p.remoteIn = inBw; p.remoteOut = outBw;
        p.control.inReliable = seq;
        queueAck(p, kControl, seq, sent);
        std::vector<u8> v;
        p16(v, p.index); p8(v, channels); p16(v, p.mtu); p32(v, connectId); p32(v, cfg.inBandwidth); p32(v, cfg.outBandwidth);
        queueReliable(p, kVerify, kControl, std::move(v));
    }

    void handleAckCmd(P& p, u8 chIdx, u16 seq, u16 echoed) {
        Channel* ch = p.chan(chIdx);
        if (!ch) return;
        auto it = std::find_if(p.sentReliable.begin(), p.sentReliable.end(), [&](const OutCmd& c) { return c.channel == chIdx && c.seq == seq; });
        if (it == p.sentReliable.end()) return;
        updateRtt(p, static_cast<u16>(t16(now) - echoed));
        const u8 type = it->type & kTypeMask;
        p.sentReliable.erase(it);
        if (ch->inFlight) --ch->inFlight;
        if (type == kVerify && p.state == PeerState::AcknowledgingConnect) {
            p.state = PeerState::Connected;
            emit(EventType::Connect, p, 0, p.connectData, DisconnectReason::None);
        } else if (type == kConnect && p.state == PeerState::Connecting) {
            p.connectAckedAt = now;
        } else if (type == kDisconnect && p.state == PeerState::Disconnecting) {
            finish(p, DisconnectReason::Local, 0, true);
        }
    }

    // Marks an unsequenced group as seen; false when it is a duplicate or too old.
    static bool acceptUnseq(P& p, u16 id) {
        const i16 delta = static_cast<i16>(static_cast<u16>(id - p.unseqHighest));
        if (delta > 0) {
            if (delta >= static_cast<i16>(kUnseqWindow)) p.unseqSeen.reset();
            else for (int k = 1; k <= delta; ++k) p.unseqSeen.reset(static_cast<u16>(p.unseqHighest + k) % kUnseqWindow);
            p.unseqHighest = id;
            p.unseqSeen.set(id % kUnseqWindow);
            return true;
        }
        if (delta <= -static_cast<i16>(kUnseqWindow)) return false;
        const usize bit = id % kUnseqWindow;
        if (p.unseqSeen.test(bit)) return false;
        p.unseqSeen.set(bit);
        return true;
    }

    void handleCommand(P& p, u8 type, u8 chIdx, u16 seq, std::vector<u8>&& payload, u16 sent) {
        const u8 cmd = type & kTypeMask;
        if (p.state == PeerState::AcknowledgingConnect && cmd >= kPing && cmd <= kBandwidth) {   // the client speaks, so it has the VERIFY even if its ack was lost
            auto it = std::find_if(p.sentReliable.begin(), p.sentReliable.end(), [](const OutCmd& c) { return (c.type & kTypeMask) == kVerify; });
            if (it != p.sentReliable.end()) {
                p.sentReliable.erase(it);
                if (p.control.inFlight) --p.control.inFlight;
            }
            p.state = PeerState::Connected;
            emit(EventType::Connect, p, 0, p.connectData, DisconnectReason::None);
        }
        if (!allowed(p.state, cmd)) return;
        if (cmd == kAck) {
            Rd r(payload.data(), payload.size());
            const u16 echoed = r.g16();
            if (!r.bad) handleAckCmd(p, chIdx, seq, echoed);
            return;
        }
        if (cmd == kDisconnect && !(type & kAckBit)) {         // the unreliable notice of disconnectNow
            Rd r(payload.data(), payload.size());
            const u32 data = r.g32();
            const bool pre = p.state == PeerState::Connecting;
            if (p.state == PeerState::Disconnecting) finish(p, DisconnectReason::Local, 0, true);
            else zombify(p, pre ? DisconnectReason::ConnectFailed : DisconnectReason::Remote, data);
            return;
        }
        if (type & kAckBit) {
            Channel* ch = p.chan(chIdx);
            if (!ch) return;
            const u16 delta = static_cast<u16>(seq - static_cast<u16>(ch->inReliable + 1));
            if (delta >= 0x8000) { queueAck(p, chIdx, seq, sent); return; }   // already delivered: the ack was lost
            if (delta >= kRecvWindow) return;
            queueAck(p, chIdx, seq, sent);
            if (delta != 0) { ch->pending.try_emplace(seq, InCmd{type, std::move(payload)}); return; }
            ch->inReliable = seq;
            deliverReliable(p, chIdx, cmd, payload);
            while (p.live() && p.state != PeerState::Zombie) {
                const u16 next = static_cast<u16>(ch->inReliable + 1);
                auto it = ch->pending.find(next);
                if (it == ch->pending.end()) break;
                InCmd c = std::move(it->second);
                ch->pending.erase(it);
                ch->inReliable = next;
                deliverReliable(p, chIdx, c.type & kTypeMask, c.payload);
            }
            return;
        }
        if (p.state != PeerState::Connected && p.state != PeerState::DisconnectLater) return;
        Channel* ch = p.chan(chIdx);
        if (!ch || chIdx == kControl) return;
        Rd r(payload.data(), payload.size());
        if (cmd == kSendUnreliable) {
            const u16 s = r.g16();
            if (r.bad || !seqNewer(s, ch->inUnreliable)) return;                 // stale
            ch->inUnreliable = s;
            emit(EventType::Receive, p, chIdx, 0, DisconnectReason::None, std::vector<u8>(payload.begin() + 2, payload.end()));
        } else if (cmd == kSendUnsequenced) {
            const u16 g = r.g16();
            if (r.bad || !acceptUnseq(p, g)) return;
            emit(EventType::Receive, p, chIdx, kFlagUnsequenced, DisconnectReason::None, std::vector<u8>(payload.begin() + 2, payload.end()));
        } else if (cmd == kSendUnrelFragment) {
            const bool unseq = (type & kUnseqBit) != 0;
            const u16 group = r.g16();
            const u32 count = r.g16();
            const u32 num = r.g16();
            const u32 total = r.g32();
            const u32 off = r.g32();
            const usize dl = r.left();
            const u8* dp = r.take(dl);
            if (r.bad || !fragmentOk(count, num, total, off, dl)) return;
            if (!unseq && !seqNewer(group, ch->inUnreliable)) return;
            Assembly* a = nullptr;
            for (Assembly& x : ch->unrelAsm) if (x.group == group && x.unseq == unseq) { a = &x; break; }
            if (!a) {
                if (ch->unrelAsm.size() >= kMaxAssemblies) ch->unrelAsm.pop_front();
                ch->unrelAsm.emplace_back();
                a = &ch->unrelAsm.back();
                a->got.assign(count, false); a->remaining = count; a->total = total; a->group = group; a->unseq = unseq;
            }
            if (a->total != total || a->got.size() != count || a->got[num]) return;
            a->got[num] = true;
            if (dl) {
                if (a->data.size() < off + dl) a->data.resize(off + dl);
                std::memcpy(a->data.data() + off, dp, dl);
                a->received += static_cast<u32>(dl);
            }
            if (--a->remaining != 0) return;
            std::vector<u8> whole = std::move(a->data);
            const bool full = a->received == total;
            ch->unrelAsm.erase(std::remove_if(ch->unrelAsm.begin(), ch->unrelAsm.end(), [&](const Assembly& x) { return x.group == group && x.unseq == unseq; }), ch->unrelAsm.end());
            if (!full) return;                     // the fragments did not add up to the size they claimed
            whole.resize(total);
            if (unseq) { if (!acceptUnseq(p, group)) return; }
            else { if (!seqNewer(group, ch->inUnreliable)) return; ch->inUnreliable = group; }
            emit(EventType::Receive, p, chIdx, unseq ? kFlagUnsequenced : 0, DisconnectReason::None, std::move(whole));
        }
    }

    void deliverReliable(P& p, u8 chIdx, u8 cmd, const std::vector<u8>& payload) {
        Rd r(payload.data(), payload.size());
        switch (cmd) {
            case kPing: break;
            case kVerify: {
                const u16 serverIndex = r.g16();
                u32 channels = r.g8();
                u32 mtu = r.g16();
                const u32 connectId = r.g32();
                const u32 inBw = r.g32();
                const u32 outBw = r.g32();
                if (r.bad || p.state != PeerState::Connecting || connectId != p.connectId || channels == 0) break;
                channels = std::min(channels, p.requestedChannels);
                p.remoteIndex = serverIndex;
                p.channels.assign(channels, Channel{});
                p.mtu = std::min<u32>(std::clamp<u32>(mtu, 576, kMaxDatagram), cfg.mtu);
                p.remoteIn = inBw; p.remoteOut = outBw;
                p.state = PeerState::Connected;
                emit(EventType::Connect, p, 0, 0, DisconnectReason::None);
                break;
            }
            case kDisconnect: {
                const u32 data = r.g32();
                if (p.state == PeerState::Disconnecting) { finish(p, DisconnectReason::Local, 0, true); break; }
                zombify(p, p.state == PeerState::Connecting ? DisconnectReason::ConnectFailed : DisconnectReason::Remote, data);
                break;
            }
            case kBandwidth: {
                const u32 inBw = r.g32();
                const u32 outBw = r.g32();
                if (!r.bad) { p.remoteIn = inBw; p.remoteOut = outBw; }
                break;
            }
            case kSendReliable:
                if (chIdx != kControl && chIdx < p.channels.size() && p.state != PeerState::Connecting) emit(EventType::Receive, p, chIdx, kFlagReliable, DisconnectReason::None, std::vector<u8>(payload));
                break;
            case kSendFragment: {
                if (chIdx == kControl || chIdx >= p.channels.size()) break;
                Channel& ch = p.channels[chIdx];
                const u16 start = r.g16();
                const u32 count = r.g32();
                const u32 num = r.g32();
                const u32 total = r.g32();
                const u32 off = r.g32();
                const usize dl = r.left();
                const u8* dp = r.take(dl);
                if (r.bad || !fragmentOk(count, num, total, off, dl)) break;
                auto it = ch.relAsm.find(start);
                if (it == ch.relAsm.end()) {
                    if (ch.relAsm.size() >= 8) break;
                    Assembly a;
                    a.remaining = count; a.total = total;
                    it = ch.relAsm.emplace(start, std::move(a)).first;
                }
                Assembly& a = it->second;
                if (a.total != total) break;
                if (dl) {
                    if (a.data.size() < off + dl) a.data.resize(off + dl);
                    std::memcpy(a.data.data() + off, dp, dl);
                    a.received += static_cast<u32>(dl);
                }
                if (--a.remaining == 0) {
                    std::vector<u8> whole = std::move(a.data);
                    const bool full = a.received == total;
                    ch.relAsm.erase(it);
                    if (!full) break;              // the fragments did not add up to the size they claimed
                    whole.resize(total);
                    emit(EventType::Receive, p, chIdx, kFlagReliable, DisconnectReason::None, std::move(whole));
                }
                break;
            }
            default: break;
        }
    }

    // ---- the service round

    void round() {
        receiveAll();
        for (auto& up : peers) {
            if (up && up->d_->live()) timers(*up->d_);
        }
        for (auto& up : peers) {
            if (up && up->d_->live()) flushPeer(*up->d_);
        }
        for (auto& up : peers) {
            if (up && up->d_->zombie) drop(*up->d_);
        }
    }
};

// ------------------------------------------------------------------ Peer

Peer::Peer() : d_(std::make_unique<Impl>()) {}
Peer::~Peer() = default;

u16 Peer::index() const { return d_->index; }
u32 Peer::generation() const { return d_->generation; }
PeerState Peer::state() const { return d_->state; }
const Address& Peer::address() const { return d_->address; }
u32 Peer::channelCount() const { return static_cast<u32>(d_->channels.size()); }
u32 Peer::mtu() const { return d_->mtu; }
u32 Peer::connectData() const { return d_->connectData; }
u32 Peer::roundTripTime() const { return d_->rtt; }
u32 Peer::roundTripTimeVariance() const { return d_->rttVar; }
u32 Peer::lastRoundTripTime() const { return d_->lastRtt; }
u32 Peer::lowestRoundTripTime() const { return d_->lowestRtt; }
u32 Peer::packetLoss() const { return d_->loss; }
u32 Peer::packetLossVariance() const { return d_->lossVar; }
u32 Peer::pingInterval() const { return d_->pingInterval; }
u64 Peer::packetsSent() const { return d_->packetsSent; }
u64 Peer::packetsLost() const { return d_->packetsLost; }
u64 Peer::packetsDropped() const { return d_->packetsDropped; }
u64 Peer::bytesSent() const { return d_->bytesSent; }
u64 Peer::bytesReceived() const { return d_->bytesReceived; }

u64 Peer::reliableDataInTransit() const {
    u64 n = 0;
    for (const OutCmd& c : d_->sentReliable) n += c.payload.size();
    for (const OutCmd& c : d_->outgoing) if (c.reliable()) n += c.payload.size();
    return n;
}

int Peer::send(u8 channel, const u8* data, usize len, u32 flags) { return d_->host->sendPacket(*d_, channel, data, len, flags); }

void Peer::ping() {
    if (d_->state == PeerState::Connected) d_->host->queueReliable(*d_, kPing, kControl, {});
}
void Peer::setPingInterval(u32 ms) { d_->pingInterval = ms ? ms : 500; }
void Peer::setTimeouts(u32 limit, u32 minimumMs, u32 maximumMs) {
    d_->timeoutLimit = limit ? limit : 32;
    d_->timeoutMin = minimumMs ? minimumMs : 5000;
    d_->timeoutMax = maximumMs ? maximumMs : 30000;
}
void Peer::throttleConfigure(u32 intervalMs, u32 acceleration, u32 deceleration) {
    d_->throttleInterval = intervalMs; d_->throttleAccel = acceleration; d_->throttleDecel = deceleration;
}
void Peer::getThrottle(u32& interval, u32& acceleration, u32& deceleration) const {
    interval = d_->throttleInterval; acceleration = d_->throttleAccel; deceleration = d_->throttleDecel;
}

void Peer::disconnect(u32 data) {
    Impl& p = *d_;
    switch (p.state) {
        case PeerState::Connected: case PeerState::DisconnectLater: p.host->startDisconnect(p, data); break;
        case PeerState::Connecting: case PeerState::AcknowledgingConnect: p.host->drop(p); break;   // never announced: forget it
        default: break;
    }
}

void Peer::disconnectLater(u32 data) {
    Impl& p = *d_;
    if (p.state != PeerState::Connected) return;
    if (p.outgoing.empty() && p.sentReliable.empty()) { p.host->startDisconnect(p, data); return; }
    p.state = PeerState::DisconnectLater;
    p.laterData = data;
}

void Peer::disconnectNow(u32 data) {
    Impl& p = *d_;
    Host::Impl& h = *p.host;
    if (!p.live()) return;
    if (p.state != PeerState::Connecting && h.sock) {
        h.tx.assign(kHeaderSize, 0);
        p8(h.tx, kDisconnect); p8(h.tx, kControl); p16(h.tx, 0); p16(h.tx, 4); p32(h.tx, data);
        h.sendDatagram(p, 1);
    }
    h.drop(p);
}

void Peer::reset() { if (d_->live()) d_->host->drop(*d_); }

// ------------------------------------------------------------------ Host

Host::Host(std::unique_ptr<DatagramSocket> socket, const HostConfig& cfg) : d_(std::make_unique<Impl>()) {
    d_->cfg = cfg;
    d_->cfg.maxPeers = std::clamp<u32>(cfg.maxPeers, 1, 4095);
    d_->cfg.mtu = std::clamp<u32>(cfg.mtu, 576, kMaxDatagram);
    d_->sock = std::move(socket);
    d_->peers.resize(d_->cfg.maxPeers);
    d_->rng = cfg.seed ? cfg.seed : static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0xA5A5A5A5DEADBEEFull;
}

Host::~Host() = default;

bool Host::hasSocket() const { return d_->sock != nullptr; }
void Host::attachSocket(std::unique_ptr<DatagramSocket> socket) { d_->sock = std::move(socket); }
DatagramSocket* Host::socket() { return d_->sock.get(); }
const HostConfig& Host::config() const { return d_->cfg; }

Peer* Host::connect(const Address& to, u32 channels, u32 connectData, u64 nowMs) {
    Impl& h = *d_;
    if (!h.sock || !to.valid()) return nullptr;
    h.now = nowMs;
    Peer* pr = h.alloc(to);
    if (!pr) return nullptr;
    Peer::Impl& p = *pr->d_;
    const u32 cap = h.channelCap();
    p.requestedChannels = channels == 0 ? cap : std::min(channels, cap);
    p.state = PeerState::Connecting;
    p.connectId = static_cast<u32>(h.rnd()) | 1u;
    p.session = static_cast<u16>(p.connectId);
    p.connectData = connectData;
    p.connectStart = nowMs;
    std::vector<u8> pl;
    p16(pl, p.index); p8(pl, p.requestedChannels); p16(pl, p.mtu); p32(pl, p.connectId); p32(pl, connectData);
    p32(pl, h.cfg.inBandwidth); p32(pl, h.cfg.outBandwidth); p16(pl, kProtoVersion);
    h.queueReliable(p, kConnect, kControl, std::move(pl));
    h.flushPeer(p);
    return pr;
}

int Host::service(Event& ev, u64 nowMs) {
    d_->now = nowMs;
    if (d_->events.empty()) d_->round();
    if (d_->events.empty()) return 0;
    ev = std::move(d_->events.front());
    d_->events.pop_front();
    return 1;
}

void Host::flush(u64 nowMs) {
    d_->now = nowMs;
    for (auto& up : d_->peers) if (up && up->d_->live()) d_->flushPeer(*up->d_);
}

void Host::broadcast(u8 channel, const u8* data, usize len, u32 flags) {
    for (auto& up : d_->peers) {
        if (up && up->d_->state == PeerState::Connected) d_->sendPacket(*up->d_, channel, data, len, flags);
    }
}

bool Host::socketSend(const Address& to, const u8* data, usize len) {
    return d_->sock && d_->sock->send(to, data, len) >= 0;
}

void Host::setBandwidthLimit(u32 inBytesPerSec, u32 outBytesPerSec) {
    d_->cfg.inBandwidth = inBytesPerSec;
    d_->cfg.outBandwidth = outBytesPerSec;
    for (auto& up : d_->peers) {
        if (!up || up->d_->state != PeerState::Connected) continue;
        std::vector<u8> pl;
        p32(pl, inBytesPerSec); p32(pl, outBytesPerSec);
        d_->queueReliable(*up->d_, kBandwidth, kControl, std::move(pl));
    }
}

void Host::setChannelLimit(u32 limit) { d_->cfg.channelLimit = std::min(limit, kMaxChannelCount); }
void Host::setRefuseConnections(bool refuse) { d_->cfg.refuseConnections = refuse; }

u32 Host::peerSlots() const { return static_cast<u32>(d_->peers.size()); }
Peer* Host::peerAt(u32 index) { return index < d_->peers.size() ? d_->peers[index].get() : nullptr; }

Peer* Host::peerByKey(u16 index, u32 generation) {
    Peer* p = peerAt(index);
    return p && p->d_->generation == generation && p->d_->live() ? p : nullptr;
}

u32 Host::connectedCount() const {
    u32 n = 0;
    for (const auto& up : d_->peers) if (up && up->d_->state == PeerState::Connected) ++n;
    return n;
}

const HostStats& Host::stats() const { return d_->stats; }

} // namespace aver::net
