#pragma once
/* Aver.Net.Abi -- the plain-C seam onto modules/net (OS sockets and the reliable-UDP host/peer protocol), so scripts and
 * managed extensions can open UDP and TCP sockets and ENet-style hosts and peers.
 * Conventions as docs/ABI.md section 13: setters return 1 on success and 0 on a rejected request (stale handle, bad argument), getters return the
 * value or a neutral one (0, -1 for "none") for a stale handle. Socket and host handles are int32 (0 invalid); a peer is named by an int64 key
 * (generation << 16 | slot), valid until that peer disconnects or its slot is reused. SINGLE GAME THREAD; nothing here locks.
 *
 * ADDRESSES cross as numeric text ("127.0.0.1", "::1"); a host name goes through aver_net_resolve first. A bind address of "*" asks for the wildcard
 * of the socket's family: only a caller that means to listen on every interface passes it. Nothing here binds a wildcard on its own: a host
 * created with no bind address opens its socket at its first connect, bound to the loopback address when the target is loopback.
 *
 * ENUMS (mirrored by hand in the extension's managed half, extensions/FabricConnector/managed/Aver.Fabric/Nodes/NetworkNative.cs, and checked by AbiEnumTest):
 *   event 0 none, 1 connect, 2 disconnect, 3 receive      reason (disconnect events) 0 none, 1 remote, 2 local, 3 timeout, 4 connect failed
 *   peer state: ENetPacketPeer.PeerState's numbering      send flags: 1 reliable, 2 unsequenced, neither = unreliable ordered
 *   disconnect mode 0 request, 1 later, 2 now             stat ids: the PeerStatistic numbering, then Aver extras from 16
 */
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_NET_ABI_BUILD)
#    define AVER_NET_API __declspec(dllexport)
#  else
#    define AVER_NET_API __declspec(dllimport)
#  endif
#else
#  define AVER_NET_API
#endif

#define AVER_NET_ABI_VERSION 1

#define AVER_NET_EVENT_NONE       0
#define AVER_NET_EVENT_CONNECT    1
#define AVER_NET_EVENT_DISCONNECT 2
#define AVER_NET_EVENT_RECEIVE    3

#define AVER_NET_REASON_NONE           0
#define AVER_NET_REASON_REMOTE         1
#define AVER_NET_REASON_LOCAL          2
#define AVER_NET_REASON_TIMEOUT        3
#define AVER_NET_REASON_CONNECT_FAILED 4

#define AVER_NET_PEER_STATE_DISCONNECTED            0
#define AVER_NET_PEER_STATE_CONNECTING              1
#define AVER_NET_PEER_STATE_ACKNOWLEDGING_CONNECT   2
#define AVER_NET_PEER_STATE_CONNECTION_PENDING      3
#define AVER_NET_PEER_STATE_CONNECTION_SUCCEEDED    4
#define AVER_NET_PEER_STATE_CONNECTED               5
#define AVER_NET_PEER_STATE_DISCONNECT_LATER        6
#define AVER_NET_PEER_STATE_DISCONNECTING           7
#define AVER_NET_PEER_STATE_ACKNOWLEDGING_DISCONNECT 8
#define AVER_NET_PEER_STATE_ZOMBIE                  9

#define AVER_NET_SEND_FLAG_RELIABLE    1
#define AVER_NET_SEND_FLAG_UNSEQUENCED 2

#define AVER_NET_DISCONNECT_MODE_REQUEST 0
#define AVER_NET_DISCONNECT_MODE_LATER   1
#define AVER_NET_DISCONNECT_MODE_NOW     2

/* Return codes of aver_net_peer_send besides 0 (ok). */
#define AVER_NET_SEND_BAD_STATE   -1
#define AVER_NET_SEND_BAD_CHANNEL -2
#define AVER_NET_SEND_TOO_BIG     -3
#define AVER_NET_SEND_BAD_HANDLE  -4

/* Return codes of the byte-moving socket calls besides a byte count. */
#define AVER_NET_WOULD_BLOCK -1
#define AVER_NET_ERROR       -2
#define AVER_NET_CLOSED      -3

/* aver_net_peer_stat ids (the first twelve are the ENet peer statistics). */
#define AVER_NET_PEERSTAT_PACKET_LOSS                 0
#define AVER_NET_PEERSTAT_PACKET_LOSS_VARIANCE        1
#define AVER_NET_PEERSTAT_PACKET_THROTTLE             2
#define AVER_NET_PEERSTAT_ROUND_TRIP_TIME             3
#define AVER_NET_PEERSTAT_ROUND_TRIP_TIME_VARIANCE    4
#define AVER_NET_PEERSTAT_LAST_ROUND_TRIP_TIME        5
#define AVER_NET_PEERSTAT_LAST_ROUND_TRIP_TIME_VARIANCE 6
#define AVER_NET_PEERSTAT_PACKET_LOSS_EPOCH           7
#define AVER_NET_PEERSTAT_PACKET_THROTTLE_EPOCH       8
#define AVER_NET_PEERSTAT_PACKET_THROTTLE_ACCELERATION 9
#define AVER_NET_PEERSTAT_PACKET_THROTTLE_DECELERATION 10
#define AVER_NET_PEERSTAT_PACKET_THROTTLE_INTERVAL    11
#define AVER_NET_PEERSTAT_BYTES_SENT                  16
#define AVER_NET_PEERSTAT_BYTES_RECEIVED              17
#define AVER_NET_PEERSTAT_PACKETS_SENT                18
#define AVER_NET_PEERSTAT_PACKETS_LOST                19
#define AVER_NET_PEERSTAT_PACKETS_DROPPED             20
#define AVER_NET_PEERSTAT_MTU                         21
#define AVER_NET_PEERSTAT_LOWEST_ROUND_TRIP_TIME      22
#define AVER_NET_PEERSTAT_RELIABLE_IN_TRANSIT         23
#define AVER_NET_PEERSTAT_PING_INTERVAL               24
#define AVER_NET_PEERSTAT_CHANNELS                    25

/* aver_net_host_stat ids. */
#define AVER_NET_HOSTSTAT_SENT_BYTES       0
#define AVER_NET_HOSTSTAT_SENT_PACKETS     1
#define AVER_NET_HOSTSTAT_RECEIVED_BYTES   2
#define AVER_NET_HOSTSTAT_RECEIVED_PACKETS 3
#define AVER_NET_HOSTSTAT_BAD_CHECKSUM     4

#ifdef __cplusplus
extern "C" {
#endif

AVER_NET_API int32_t aver_net_abi_version(void);
/* Milliseconds on a monotonic clock; the clock every host here runs on. */
AVER_NET_API int64_t aver_net_now_ms(void);

/* ---- names and interfaces ---- */
/* Resolves a name or numeric address (ipType 0 any, 4, 6) to numeric addresses joined by '\n' in out. Returns the byte count (no terminator
 * counted; the buffer is NUL-terminated), 0 when it does not resolve, or minus the size needed (with the terminator) when the buffer is too small. */
AVER_NET_API int32_t aver_net_resolve(const char* hostName, int32_t ipType, uint8_t* buffer, int32_t capacity);
AVER_NET_API int32_t aver_net_local_addresses(uint8_t* buffer, int32_t capacity);

/* ---- sockets: UDP, TCP and TCP listeners share one handle space ---- */
AVER_NET_API int32_t aver_net_udp_open(int32_t family);
AVER_NET_API int32_t aver_net_udp_bind(int32_t sock, const char* ip, int32_t port);
AVER_NET_API int32_t aver_net_udp_local_port(int32_t sock);
AVER_NET_API int32_t aver_net_udp_send(int32_t sock, const uint8_t* data, int32_t length, const char* ip, int32_t port);
AVER_NET_API int32_t aver_net_udp_recv(int32_t sock, uint8_t* buffer, int32_t capacity, uint8_t* ipOut, int32_t ipCapacity, int32_t* portOut);
AVER_NET_API int32_t aver_net_udp_wait(int32_t sock, int32_t timeoutMs);
AVER_NET_API int32_t aver_net_udp_set_broadcast(int32_t sock, int32_t enable);

AVER_NET_API int32_t aver_net_tcp_connect(const char* ip, int32_t port);
/* 0 still connecting, 1 connected, -1 failed or closed. */
AVER_NET_API int32_t aver_net_tcp_status(int32_t sock);
AVER_NET_API int32_t aver_net_tcp_send(int32_t sock, const uint8_t* data, int32_t length);
AVER_NET_API int32_t aver_net_tcp_recv(int32_t sock, uint8_t* buffer, int32_t capacity);
AVER_NET_API int32_t aver_net_tcp_wait(int32_t sock, int32_t forWrite, int32_t timeoutMs);
AVER_NET_API int32_t aver_net_tcp_set_nodelay(int32_t sock, int32_t enable);
AVER_NET_API int32_t aver_net_tcp_local_port(int32_t sock);
AVER_NET_API int32_t aver_net_tcp_remote(int32_t sock, uint8_t* ipOut, int32_t ipCapacity, int32_t* portOut);
AVER_NET_API int32_t aver_net_tcp_listen(const char* ip, int32_t port, int32_t backlog);
AVER_NET_API int32_t aver_net_tcp_accept(int32_t listener);
AVER_NET_API int32_t aver_net_tcp_listener_port(int32_t listener);
AVER_NET_API int32_t aver_net_tcp_listener_wait(int32_t listener, int32_t timeoutMs);
/* Closes a UDP socket, a TCP socket or a listener. */
AVER_NET_API int32_t aver_net_close(int32_t handle);

/* ---- hosts and peers ---- */
/* bindIp NULL or "" makes a client host: its socket opens at the first connect. maxPeers 1..4095, channelLimit 0 = 255, bandwidths in bytes per
 * second (0 = unlimited). Returns the host handle, 0 on failure (a taken port, a bad address). */
AVER_NET_API int32_t aver_net_host_create(const char* bindIp, int32_t port, int32_t maxPeers, int32_t channelLimit, int32_t inBandwidth, int32_t outBandwidth);
AVER_NET_API int32_t aver_net_host_destroy(int32_t host);
AVER_NET_API int32_t aver_net_host_local_port(int32_t host);
AVER_NET_API int32_t aver_net_host_max_peers(int32_t host);
AVER_NET_API int32_t aver_net_host_channel_limit(int32_t host, int32_t limit);
AVER_NET_API int32_t aver_net_host_bandwidth_limit(int32_t host, int32_t inBandwidth, int32_t outBandwidth);
AVER_NET_API int32_t aver_net_host_refuse(int32_t host, int32_t refuse);
/* The peer key, or 0 (every slot used, bad address, no socket). channels 0 = as many as the host allows; data is the connect-data word. */
AVER_NET_API int64_t aver_net_host_connect(int32_t host, const char* ip, int32_t port, int32_t channels, uint32_t data);
/* Runs the host once (waiting up to timeoutMs for traffic when nothing is pending) and fills the next event. Returns 1 with an event, 0 without,
 * -1 for a stale handle. For a receive event length is the packet size (fetch it with aver_net_host_event_data before the next call) and data is
 * the packet's send flags. */
AVER_NET_API int32_t aver_net_host_service(int32_t host, int32_t timeoutMs, int32_t* type, int64_t* peer, int32_t* channel, uint32_t* data, int32_t* reason, int32_t* length);
AVER_NET_API int32_t aver_net_host_event_data(int32_t host, uint8_t* buffer, int32_t capacity);
AVER_NET_API int32_t aver_net_host_flush(int32_t host);
AVER_NET_API int32_t aver_net_host_broadcast(int32_t host, int32_t channel, const uint8_t* data, int32_t length, int32_t flags);
AVER_NET_API int32_t aver_net_host_socket_send(int32_t host, const char* ip, int32_t port, const uint8_t* data, int32_t length);
/* Writes up to capacity keys of the connected peers; returns how many are connected. */
AVER_NET_API int32_t aver_net_host_peers(int32_t host, int64_t* keys, int32_t capacity);
AVER_NET_API double  aver_net_host_stat(int32_t host, int32_t which);

AVER_NET_API int32_t aver_net_peer_send(int32_t host, int64_t peer, int32_t channel, const uint8_t* data, int32_t length, int32_t flags);
AVER_NET_API int32_t aver_net_peer_state(int32_t host, int64_t peer);
AVER_NET_API int32_t aver_net_peer_disconnect(int32_t host, int64_t peer, uint32_t data, int32_t mode);
AVER_NET_API int32_t aver_net_peer_reset(int32_t host, int64_t peer);
AVER_NET_API int32_t aver_net_peer_ping(int32_t host, int64_t peer);
AVER_NET_API int32_t aver_net_peer_set_ping_interval(int32_t host, int64_t peer, int32_t intervalMs);
AVER_NET_API int32_t aver_net_peer_set_timeout(int32_t host, int64_t peer, int32_t limit, int32_t minimumMs, int32_t maximumMs);
AVER_NET_API int32_t aver_net_peer_throttle(int32_t host, int64_t peer, int32_t intervalMs, int32_t acceleration, int32_t deceleration);
AVER_NET_API int32_t aver_net_peer_address(int32_t host, int64_t peer, uint8_t* ipOut, int32_t ipCapacity, int32_t* portOut);
AVER_NET_API uint32_t aver_net_peer_connect_data(int32_t host, int64_t peer);
AVER_NET_API double  aver_net_peer_stat(int32_t host, int64_t peer, int32_t which);

#ifdef __cplusplus
}
#endif
