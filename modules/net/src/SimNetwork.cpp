#include "aver/net/Datagram.hpp"

#include <cstring>

namespace aver::net {

SimNetwork::~SimNetwork() {
    for (auto& kv : sockets_) kv.second->net_ = nullptr;
}

u64 SimNetwork::rnd() {                       // splitmix64
    rng_ += 0x9E3779B97F4A7C15ull;
    u64 z = rng_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::unique_ptr<DatagramSocket> SimNetwork::bind(Address at) {
    if (!at.valid()) return nullptr;
    if (at.port == 0) {
        do { at.port = nextPort_++; } while (sockets_.count(at));
    } else if (sockets_.count(at)) {
        return nullptr;
    }
    auto s = std::make_unique<SimSocket>();
    s->net_ = this;
    s->addr_ = at;
    sockets_[at] = s.get();
    return s;
}

void SimNetwork::post(const Address& from, const Address& to, const u8* data, usize len) {
    ++stats.sent;
    const auto it = links_.find({from, to});
    const LinkConfig& c = it != links_.end() ? it->second : config;
    if (len > c.maxDatagram || dead_.count(from) || dead_.count(to) || unit() < c.lossProbability) { ++stats.dropped; return; }
    int copies = 1;
    if (unit() < c.duplicateProbability) { copies = 2; ++stats.duplicated; }
    for (int i = 0; i < copies; ++i) {
        const u32 span = c.maxDelayMs > c.minDelayMs ? c.maxDelayMs - c.minDelayMs : 0;
        const u64 delay = c.minDelayMs + (span ? rnd() % (span + 1) : 0);
        wire_.push(InFlight{now_ + delay, seq_++, from, to, std::vector<u8>(data, data + len)});
    }
}

void SimNetwork::deliverDue() {
    while (!wire_.empty() && wire_.top().at <= now_) {
        InFlight f = wire_.top();
        wire_.pop();
        const auto it = sockets_.find(f.to);
        if (it == sockets_.end() || dead_.count(f.to) || dead_.count(f.from)) { ++stats.dropped; continue; }
        it->second->inbox_.push(SimSocket::Msg{f.from, std::move(f.bytes)});
        ++stats.delivered;
    }
}

SimSocket::~SimSocket() {
    if (net_) net_->sockets_.erase(addr_);
}

int SimSocket::send(const Address& to, const u8* data, usize len) {
    if (!net_) return kSockError;
    net_->post(addr_, to, data, len);
    return static_cast<int>(len);
}

int SimSocket::recv(Address& from, u8* buf, usize cap) {
    if (!net_) return kSockError;
    net_->deliverDue();
    if (inbox_.empty()) return kWouldBlock;
    Msg m = std::move(inbox_.front());
    inbox_.pop();
    const usize n = m.bytes.size() < cap ? m.bytes.size() : cap;
    std::memcpy(buf, m.bytes.data(), n);
    from = m.from;
    return static_cast<int>(n);
}

} // namespace aver::net
