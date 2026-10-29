// The C seam (net_abi.h) end to end over loopback: name resolution, UDP and TCP handles, a host pair with events and peer calls, stale handles.
// LOOPBACK ONLY, like NetSocketTest: every bind is 127.0.0.1 with port 0, no wildcard, short waits.
#include "aver/core/Log.hpp"
#include "aver/net/net_abi.h"

#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

struct Ev { int32_t type = 0, channel = 0, reason = 0, length = 0; int64_t peer = 0; uint32_t data = 0; std::vector<uint8_t> packet; };

static std::vector<Ev> drain(int32_t host, int timeoutMs = 0) {
    std::vector<Ev> out;
    for (;;) {
        Ev e;
        const int r = aver_net_host_service(host, out.empty() ? timeoutMs : 0, &e.type, &e.peer, &e.channel, &e.data, &e.reason, &e.length);
        if (r != 1) break;
        if (e.type == AVER_NET_EVENT_RECEIVE) {
            e.packet.resize(static_cast<size_t>(e.length));
            aver_net_host_event_data(host, e.packet.data(), e.length);
        }
        out.push_back(std::move(e));
    }
    return out;
}

static bool wait(const std::vector<int32_t>& hosts, std::vector<std::vector<Ev>>& logs, const std::function<bool()>& done, int limitMs = 5000) {
    const int64_t end = aver_net_now_ms() + limitMs;
    while (aver_net_now_ms() < end) {
        for (size_t i = 0; i < hosts.size(); ++i) for (Ev& e : drain(hosts[i], 2)) logs[i].push_back(std::move(e));
        if (done()) return true;
    }
    return done();
}

static size_t count(const std::vector<Ev>& v, int type) { size_t n = 0; for (const Ev& e : v) n += e.type == type; return n; }

int main() {
    AVER_INFO("NetAbiTest");
    check(aver_net_abi_version() >= 1, "abi version");
    check(aver_net_now_ms() > 0, "clock");

    uint8_t text[512];
    int n = aver_net_resolve("localhost", 4, text, sizeof(text));
    check(n > 0 && std::strstr(reinterpret_cast<char*>(text), "127.") == reinterpret_cast<char*>(text), "resolve(localhost, 4) gives a loopback address");
    check(aver_net_resolve("::1", 0, text, sizeof(text)) == 3 && std::strcmp(reinterpret_cast<char*>(text), "::1") == 0, "numeric text resolves to itself");
    check(aver_net_resolve("127.0.0.1", 6, text, sizeof(text)) == 0, "a v4 address is not a v6 one");
    check(aver_net_resolve("127.0.0.1", 0, text, 4) < 0, "a small buffer is answered with minus the size needed");
    check(aver_net_local_addresses(text, sizeof(text)) > 0, "local addresses");

    // UDP
    const int32_t a = aver_net_udp_open(4), b = aver_net_udp_open(4);
    check(a && b && a != b, "two UDP handles");
    check(aver_net_udp_bind(a, "127.0.0.1", 0) == 1 && aver_net_udp_bind(b, "127.0.0.1", 0) == 1, "bound to loopback");
    const int32_t pb = aver_net_udp_local_port(b);
    check(pb > 0 && aver_net_udp_local_port(a) > 0, "ports");
    uint8_t buf[256];
    uint8_t ip[64];
    int32_t port = 0;
    check(aver_net_udp_recv(b, buf, sizeof(buf), ip, sizeof(ip), &port) == AVER_NET_WOULD_BLOCK, "empty");
    const uint8_t msg[5] = {1, 2, 3, 4, 5};
    check(aver_net_udp_send(a, msg, 5, "127.0.0.1", pb) == 5, "send");
    check(aver_net_udp_wait(b, 2000) == 1, "wait");
    check(aver_net_udp_recv(b, buf, sizeof(buf), ip, sizeof(ip), &port) == 5 && std::memcmp(buf, msg, 5) == 0 && std::strcmp(reinterpret_cast<char*>(ip), "127.0.0.1") == 0 && port == aver_net_udp_local_port(a), "received with the sender's address");
    check(aver_net_udp_send(a, msg, 5, "not-an-address", 1) < 0, "a name is not accepted where numeric text is required");
    check(aver_net_close(a) == 1 && aver_net_close(a) == 0 && aver_net_udp_local_port(a) == 0, "close, then the handle is stale");
    aver_net_close(b);

    // TCP
    const int32_t lis = aver_net_tcp_listen("127.0.0.1", 0, 4);
    check(lis != 0 && aver_net_tcp_listener_port(lis) > 0, "listener");
    const int32_t cli = aver_net_tcp_connect("127.0.0.1", aver_net_tcp_listener_port(lis));
    check(cli != 0, "connect");
    check(aver_net_tcp_listener_wait(lis, 2000) == 1, "listener wakes");
    const int32_t srv = aver_net_tcp_accept(lis);
    check(srv != 0, "accept");
    aver_net_tcp_wait(cli, 1, 2000);
    check(aver_net_tcp_status(cli) == 1, "connected");
    check(aver_net_tcp_set_nodelay(cli, 1) == 1, "nodelay");
    check(aver_net_tcp_send(cli, msg, 5) == 5, "tcp send");
    aver_net_tcp_wait(srv, 0, 2000);
    check(aver_net_tcp_recv(srv, buf, sizeof(buf)) == 5 && std::memcmp(buf, msg, 5) == 0, "tcp recv");
    check(aver_net_tcp_remote(srv, ip, sizeof(ip), &port) == 1 && std::strcmp(reinterpret_cast<char*>(ip), "127.0.0.1") == 0 && port == aver_net_tcp_local_port(cli), "remote address");
    aver_net_close(srv);
    int r = AVER_NET_WOULD_BLOCK;
    for (int i = 0; i < 100 && r == AVER_NET_WOULD_BLOCK; ++i) { aver_net_tcp_wait(cli, 0, 20); r = aver_net_tcp_recv(cli, buf, sizeof(buf)); }
    check(r == AVER_NET_CLOSED, "close is seen");
    aver_net_close(cli);
    aver_net_close(lis);

    // hosts
    const int32_t sh = aver_net_host_create("127.0.0.1", 0, 8, 4, 0, 0);
    const int32_t ch = aver_net_host_create(nullptr, 0, 1, 0, 0, 0);
    check(sh && ch, "a bound server host and an unbound client host");
    const int32_t sport = aver_net_host_local_port(sh);
    check(sport > 0 && aver_net_host_local_port(ch) == 0, "the client host has no socket until it connects");
    check(aver_net_host_max_peers(sh) == 8, "max peers");
    check(aver_net_host_connect(ch, "127.0.0.1", 0, 2, 0) == 0, "port 0 is not a destination");
    const int64_t peer = aver_net_host_connect(ch, "127.0.0.1", sport, 6, 0x5EED);
    check(peer != 0, "connect");
    check(aver_net_host_local_port(ch) > 0, "now it has a loopback socket");
    std::vector<int32_t> hosts = {sh, ch};
    std::vector<std::vector<Ev>> logs(2);
    check(wait(hosts, logs, [&] { return count(logs[0], AVER_NET_EVENT_CONNECT) == 1 && count(logs[1], AVER_NET_EVENT_CONNECT) == 1; }), "both sides connect");
    const int64_t speer = logs[0][0].peer;
    check(logs[0][0].data == 0x5EED, "the connect-data word crosses");
    check(aver_net_peer_state(sh, speer) == AVER_NET_PEER_STATE_CONNECTED && aver_net_peer_state(ch, peer) == AVER_NET_PEER_STATE_CONNECTED, "peer state");
    check(aver_net_peer_stat(ch, peer, AVER_NET_PEERSTAT_CHANNELS) == 4, "the server's channel limit (4) caps the request (6)");
    check(aver_net_peer_connect_data(sh, speer) == 0x5EED, "connect data on the peer");
    check(aver_net_peer_address(sh, speer, ip, sizeof(ip), &port) == 1 && std::strcmp(reinterpret_cast<char*>(ip), "127.0.0.1") == 0, "peer address");
    int64_t keys[4] = {};
    check(aver_net_host_peers(sh, keys, 4) == 1 && keys[0] == speer, "peers");

    const uint8_t rel[3] = {9, 8, 7};
    check(aver_net_peer_send(ch, peer, 1, rel, 3, AVER_NET_SEND_FLAG_RELIABLE) == 0, "reliable send");
    check(aver_net_peer_send(ch, peer, 5, rel, 3, 0) == AVER_NET_SEND_BAD_CHANNEL, "a channel past the count is refused");
    check(aver_net_peer_send(ch, peer + (1ll << 20), 0, rel, 3, 0) == AVER_NET_SEND_BAD_HANDLE, "a stale key is refused");
    check(wait(hosts, logs, [&] { return count(logs[0], AVER_NET_EVENT_RECEIVE) == 1; }), "received");
    for (const Ev& e : logs[0]) if (e.type == AVER_NET_EVENT_RECEIVE) check(e.channel == 1 && e.packet == std::vector<uint8_t>({9, 8, 7}), "payload and channel");
    check(aver_net_host_broadcast(sh, 0, rel, 3, AVER_NET_SEND_FLAG_RELIABLE) == 1, "broadcast");
    check(wait(hosts, logs, [&] { return count(logs[1], AVER_NET_EVENT_RECEIVE) == 1; }), "client receives it");
    check(aver_net_host_stat(ch, AVER_NET_HOSTSTAT_SENT_PACKETS) > 0 && aver_net_host_stat(sh, AVER_NET_HOSTSTAT_RECEIVED_BYTES) > 0, "host statistics");
    check(aver_net_peer_set_timeout(ch, peer, 8, 2000, 10000) == 1 && aver_net_peer_set_ping_interval(ch, peer, 250) == 1 && aver_net_peer_throttle(ch, peer, 5000, 3, 4) == 1, "peer tuning");
    check(aver_net_peer_stat(ch, peer, AVER_NET_PEERSTAT_PACKET_THROTTLE_ACCELERATION) == 3 && aver_net_peer_stat(ch, peer, AVER_NET_PEERSTAT_PING_INTERVAL) == 250, "and reads back");
    check(aver_net_host_bandwidth_limit(sh, 100000, 100000) == 1, "bandwidth limit");

    check(aver_net_peer_disconnect(ch, peer, 31, AVER_NET_DISCONNECT_MODE_REQUEST) == 1, "disconnect");
    check(wait(hosts, logs, [&] { return count(logs[0], AVER_NET_EVENT_DISCONNECT) == 1 && count(logs[1], AVER_NET_EVENT_DISCONNECT) == 1; }), "both sides see the Disconnect");
    for (const Ev& e : logs[0]) if (e.type == AVER_NET_EVENT_DISCONNECT) check(e.data == 31 && e.reason == AVER_NET_REASON_REMOTE, "data word and reason");
    check(aver_net_peer_state(ch, peer) == AVER_NET_PEER_STATE_DISCONNECTED && aver_net_peer_send(ch, peer, 0, rel, 3, 0) == AVER_NET_SEND_BAD_HANDLE, "the key is stale afterwards");

    // refusing connections turns a client away; the client gives up
    aver_net_host_refuse(sh, 1);
    const int64_t p2 = aver_net_host_connect(ch, "127.0.0.1", sport, 2, 0);
    check(p2 != 0 && aver_net_peer_set_timeout(ch, p2, 4, 500, 1500) == 1, "connect to a refusing server");
    check(wait(hosts, logs, [&] { return count(logs[1], AVER_NET_EVENT_DISCONNECT) == 2; }, 8000), "which ends in a Disconnect event");
    for (size_t i = logs[1].size(); i-- > 0;) if (logs[1][i].type == AVER_NET_EVENT_DISCONNECT) { check(logs[1][i].reason == AVER_NET_REASON_CONNECT_FAILED, "reason connect-failed"); break; }

    check(aver_net_host_destroy(sh) == 1 && aver_net_host_destroy(sh) == 0 && aver_net_host_destroy(ch) == 1, "destroy");
    check(aver_net_host_service(sh, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == -1, "a destroyed host handle is stale");

    if (g_failures) { AVER_ERROR("{} check(s) failed", g_failures); return 1; }
    AVER_INFO("all checks passed");
    return 0;
}
