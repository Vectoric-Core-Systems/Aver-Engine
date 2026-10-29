#pragma once
// The OS socket headers behind one spelling, private to Aver.Net.
#include "aver/net/Socket.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#include <cstring>

namespace aver::net::detail {

#if defined(_WIN32)
using Fd = SOCKET;
inline constexpr Fd kBadFd = INVALID_SOCKET;
inline int lastError() { return WSAGetLastError(); }
inline bool isWouldBlock(int e) { return e == WSAEWOULDBLOCK; }
inline bool isInProgress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
inline void closeFd(Fd f) { ::closesocket(f); }
#else
using Fd = int;
inline constexpr Fd kBadFd = -1;
inline int lastError() { return errno; }
inline bool isWouldBlock(int e) { return e == EWOULDBLOCK || e == EAGAIN; }
inline bool isInProgress(int e) { return e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN; }
inline void closeFd(Fd f) { ::close(f); }
#endif

inline Fd toFd(NativeSocket s) { return static_cast<Fd>(s); }
inline NativeSocket fromFd(Fd f) { return f == kBadFd ? -1 : static_cast<NativeSocket>(f); }

// Starts the socket layer (WSAStartup on Windows) once; every socket call goes through it.
void init();
bool setNonBlocking(Fd f);

// Address <-> sockaddr.
int toSockaddr(const Address& a, sockaddr_storage& ss);
bool fromSockaddr(const sockaddr_storage& ss, Address& a);

} // namespace aver::net::detail
