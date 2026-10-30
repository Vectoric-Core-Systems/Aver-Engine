#pragma once
// A reliable-UDP host/peer protocol with ENet-like semantics (docs/plans/fabric/G6_NETWORK.md, NET1). Not wire compatible with ENet.
//
// A Host owns one datagram socket and up to maxPeers Peers. connect() makes a client peer; a datagram from an unknown address with a
// CONNECT makes a server peer. Packets travel on numbered channels, reliable (acknowledged, resent, delivered in order per channel),
// unreliable-ordered (the default: stale ones are dropped) or unsequenced (no order, duplicates dropped); a packet bigger than the
// path MTU is fragmented and reassembled. The host has no clock of its own: every call that moves time takes `nowMs`, so the same
// code runs on the real clock and on SimNetwork's virtual one.
#include "aver/net/Datagram.hpp"

#include <memory>
#include <vector>

namespace aver::net {

inline constexpr u32 kFlagReliable    = 1;   // acknowledged and ordered per channel
inline constexpr u32 kFlagUnsequenced = 2;   // unreliable and unordered; ignored with kFlagReliable. Neither flag = unreliable ordered
inline constexpr usize kMaxPacketSize = 32u << 20;
inline constexpr u32 kMaxChannelCount = 255;
inline constexpr u32 kPacketLossScale = 65536;

// Numbering is the ENet peer state numbering.
enum class PeerState : int {
    Disconnected = 0, Connecting = 1, AcknowledgingConnect = 2, ConnectionPending = 3, ConnectionSucceeded = 4,
    Connected = 5, DisconnectLater = 6, Disconnecting = 7, AcknowledgingDisconnect = 8, Zombie = 9,
};

enum class EventType : int { None = 0, Connect = 1, Disconnect = 2, Receive = 3 };

enum class DisconnectReason : int {
    None = 0,
    Remote = 1,         // the other side asked
    Local = 2,          // our own disconnect finished
    Timeout = 3,        // a connected peer stopped answering
    ConnectFailed = 4,  // a connection that never completed
};

// Return codes of Peer::send.
inline constexpr int kSendOk = 0, kSendBadState = -1, kSendBadChannel = -2, kSendTooBig = -3;

struct HostConfig {
    u32 maxPeers = 32;           // 1..4095
    u32 channelLimit = 0;        // most channels a peer may use; 0 = kMaxChannelCount
    u32 inBandwidth = 0;         // bytes per second the host can take in; 0 = unlimited. Advertised: peers never send faster than this
    u32 outBandwidth = 0;        // bytes per second the host sends; 0 = unlimited
    u32 mtu = 1200;              // largest datagram, 576..4096; a connection uses the smaller of the two ends
    bool checksum = true;        // CRC-32 on every datagram; both ends must agree
    bool refuseConnections = false;
    u64 seed = 0;                // connection ids; 0 = from the clock
};

class Host;
class Peer;

struct Event {
    EventType type = EventType::None;
    Peer* peer = nullptr;        // the slot; valid while the host lives (a Disconnect event's peer is already Disconnected)
    u16 peerIndex = 0xFFFF;
    u32 peerGeneration = 0;      // bumped each time the slot is reused or reset
    u8 channel = 0;              // Receive
    u32 data = 0;                // Connect: the connect-data word; Disconnect: the disconnect data word; Receive: the packet's send flags (kFlagReliable, kFlagUnsequenced or 0)
    DisconnectReason reason = DisconnectReason::None;
    std::vector<u8> packet;      // Receive
};

class Peer {
public:
    ~Peer();
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    u16 index() const;
    u32 generation() const;
    PeerState state() const;
    bool isActive() const { return state() == PeerState::Connected; }
    const Address& address() const;
    u32 channelCount() const;
    u32 mtu() const;
    u32 connectData() const;
    u32 roundTripTime() const;               // ms, smoothed
    u32 roundTripTimeVariance() const;       // ms: the jitter estimate
    u32 lastRoundTripTime() const;
    u32 lowestRoundTripTime() const;
    u32 packetLoss() const;                  // fraction of resent reliable commands, scaled by kPacketLossScale
    u32 packetLossVariance() const;
    u32 pingInterval() const;
    u64 packetsSent() const;
    u64 packetsLost() const;
    u64 packetsDropped() const;              // unreliable packets thrown away by a bandwidth limit
    u64 bytesSent() const;
    u64 bytesReceived() const;
    u64 reliableDataInTransit() const;       // payload bytes sent and not yet acknowledged, plus queued

    // 0 ok, kSendBadState (not connected), kSendBadChannel, kSendTooBig.
    int send(u8 channel, const u8* data, usize len, u32 flags);
    void ping();
    void setPingInterval(u32 ms);
    void setTimeouts(u32 limit, u32 minimumMs, u32 maximumMs);   // default 32, 5000, 30000
    void throttleConfigure(u32 intervalMs, u32 acceleration, u32 deceleration);   // stored and reported; no RTT throttle is applied
    void getThrottle(u32& interval, u32& acceleration, u32& deceleration) const;
    void disconnect(u32 data);               // queued reliable request; unsent and unacknowledged data are discarded; Disconnect event when acked
    void disconnectLater(u32 data);          // after everything queued has been delivered
    void disconnectNow(u32 data);            // one unreliable notice, then the slot is reset with no local event
    void reset();                            // forget the peer silently

    // Made and owned by a Host; the pieces below are public only so the host's implementation file needs no friendship across nested types.
    Peer();
    struct Impl;
    std::unique_ptr<Impl> d_;
};

struct HostStats {
    u64 sentBytes = 0, sentPackets = 0, receivedBytes = 0, receivedPackets = 0;
    u64 badChecksum = 0, malformed = 0;
};

class Host {
public:
    // Takes the socket, or none for a client host that makes one at its first connect() (see attachSocket).
    Host(std::unique_ptr<DatagramSocket> socket, const HostConfig& cfg);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    bool hasSocket() const;
    void attachSocket(std::unique_ptr<DatagramSocket> socket);
    DatagramSocket* socket();
    const HostConfig& config() const;

    // A client peer; null when every slot is used or there is no socket. channels 0 = as many as the host allows.
    Peer* connect(const Address& to, u32 channels, u32 connectData, u64 nowMs);

    // Receive and send once, then hand out one event; 0 when none is pending. Call in a loop to drain.
    int service(Event& ev, u64 nowMs);
    void flush(u64 nowMs);
    void broadcast(u8 channel, const u8* data, usize len, u32 flags);
    // Sends a raw datagram from the host's socket.
    bool socketSend(const Address& to, const u8* data, usize len);

    void setBandwidthLimit(u32 inBytesPerSec, u32 outBytesPerSec);
    void setChannelLimit(u32 limit);
    void setRefuseConnections(bool refuse);

    u32 peerSlots() const;
    Peer* peerAt(u32 index);
    Peer* peerByKey(u16 index, u32 generation);
    u32 connectedCount() const;
    const HostStats& stats() const;

    struct Impl;
private:
    std::unique_ptr<Impl> d_;
};

u32 crc32(const u8* data, usize len);

} // namespace aver::net
