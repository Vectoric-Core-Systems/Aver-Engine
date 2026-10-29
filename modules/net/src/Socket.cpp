#include "aver/net/Socket.hpp"
#include "SockPlatform.hpp"

#include <chrono>

namespace aver::net {

using namespace detail;

namespace detail {
bool setNonBlocking(Fd f) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(f, FIONBIO, &on) == 0;
#else
    const int fl = fcntl(f, F_GETFL, 0);
    return fl >= 0 && fcntl(f, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
}
} // namespace detail

// macOS has no MSG_NOSIGNAL: the option keeps a write to a closed socket from raising SIGPIPE.
static void noSigpipe(Fd f) {
#if defined(SO_NOSIGPIPE)
    int on = 1;
    setsockopt(f, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&on), sizeof(on));
#else
    (void)f;
#endif
}

// Polls again until the deadline: WSAPoll wakes on the system timer tick (often 15.6 ms), so a timeout can end early,
// and a POSIX poll can be cut short by a signal.
bool waitSocket(NativeSocket s, bool forWrite, int timeoutMs) {
    if (s == -1) return false;
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0);
    int left = timeoutMs;
    for (;;) {
#if defined(_WIN32)
        WSAPOLLFD p{};
        p.fd = toFd(s);
        p.events = forWrite ? POLLWRNORM : POLLRDNORM;
        const int r = WSAPoll(&p, 1, left);
        const bool interrupted = false;
#else
        pollfd p{};
        p.fd = toFd(s);
        p.events = forWrite ? POLLOUT : POLLIN;
        const int r = ::poll(&p, 1, left);
        const bool interrupted = r < 0 && errno == EINTR;
#endif
        if (r > 0) return (p.revents & (p.events | POLLHUP | POLLERR)) != 0;
        if ((r < 0 && !interrupted) || timeoutMs == 0) return false;
        if (timeoutMs < 0) continue;   // infinite: only an interrupt gets here
        const auto now = Clock::now();
        if (now >= deadline) return false;
        left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()) + 1;
    }
}

// ---- UDP ----

bool UdpSocket::open(int family) {
    init();
    close();
    if (family != 4 && family != 6) return false;
    Fd f = ::socket(family == 4 ? AF_INET : AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (f == kBadFd) return false;
    if (!setNonBlocking(f)) { closeFd(f); return false; }
    if (family == 6) { int on = 1; setsockopt(f, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&on), sizeof(on)); }
#if defined(_WIN32)
    // A datagram sent to a closed port makes the next recvfrom fail with WSAECONNRESET; the host does not care.
    DWORD bytes = 0;
    BOOL off = FALSE;
    WSAIoctl(f, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
#endif
    s_ = fromFd(f);
    family_ = family;
    return true;
}

bool UdpSocket::bind(const Address& a) {
    if (!a.valid()) return false;
    if (!isOpen() && !open(a.family)) return false;
    if (a.family != family_) return false;
    sockaddr_storage ss;
    const int n = toSockaddr(a, ss);
    return ::bind(toFd(s_), reinterpret_cast<const sockaddr*>(&ss), n) == 0;
}

void UdpSocket::close() {
    if (s_ != -1) closeFd(toFd(s_));
    s_ = -1;
    family_ = 0;
}

Address UdpSocket::localAddress() const {
    Address a;
    if (s_ == -1) return a;
    sockaddr_storage ss{};
#if defined(_WIN32)
    int len = sizeof(ss);
#else
    socklen_t len = sizeof(ss);
#endif
    if (getsockname(toFd(s_), reinterpret_cast<sockaddr*>(&ss), &len) == 0) fromSockaddr(ss, a);
    return a;
}

bool UdpSocket::setBroadcast(bool on) {
    int v = on ? 1 : 0;
    return s_ != -1 && setsockopt(toFd(s_), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
}

bool UdpSocket::setBufferSizes(int recvBytes, int sendBytes) {
    if (s_ == -1) return false;
    bool ok = true;
    if (recvBytes > 0) ok &= setsockopt(toFd(s_), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvBytes), sizeof(recvBytes)) == 0;
    if (sendBytes > 0) ok &= setsockopt(toFd(s_), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBytes), sizeof(sendBytes)) == 0;
    return ok;
}

int UdpSocket::sendTo(const Address& to, const u8* data, usize len) {
    if (s_ == -1 || !to.valid() || to.family != family_) return kSockError;
    sockaddr_storage ss;
    const int n = toSockaddr(to, ss);
    const auto r = ::sendto(toFd(s_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0, reinterpret_cast<const sockaddr*>(&ss), n);
    if (r >= 0) return static_cast<int>(r);
    return isWouldBlock(lastError()) ? kWouldBlock : kSockError;
}

int UdpSocket::recvFrom(Address& from, u8* buf, usize cap) {
    if (s_ == -1) return kSockError;
    for (int attempt = 0; attempt < 8; ++attempt) {
        sockaddr_storage ss{};
#if defined(_WIN32)
        int len = sizeof(ss);
#else
        socklen_t len = sizeof(ss);
#endif
        const auto r = ::recvfrom(toFd(s_), reinterpret_cast<char*>(buf), static_cast<int>(cap), 0, reinterpret_cast<sockaddr*>(&ss), &len);
        if (r >= 0) { fromSockaddr(ss, from); return static_cast<int>(r); }
        const int e = lastError();
        if (isWouldBlock(e)) return kWouldBlock;
#if defined(_WIN32)
        if (e == WSAECONNRESET) continue;                 // an ICMP port-unreachable surfacing as an error: not ours
        if (e == WSAEMSGSIZE) { fromSockaddr(ss, from); return static_cast<int>(cap); }   // truncated: the data is not whole, the protocol drops it
#else
        if (e == ECONNREFUSED || e == EINTR) continue;
#endif
        return kSockError;
    }
    return kWouldBlock;
}

// ---- TCP ----

bool TcpSocket::beginConnect(const Address& to, const Address* from) {
    init();
    close();
    if (!to.valid()) return false;
    Fd f = ::socket(to.family == 4 ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (f == kBadFd) return false;
    if (!setNonBlocking(f)) { closeFd(f); return false; }
    noSigpipe(f);
    if (to.family == 6) { int on = 1; setsockopt(f, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&on), sizeof(on)); }
    if (from && from->valid() && from->family == to.family) {
        sockaddr_storage b;
        const int bn = toSockaddr(*from, b);
        if (::bind(f, reinterpret_cast<const sockaddr*>(&b), bn) != 0) { closeFd(f); return false; }
    }
    sockaddr_storage ss;
    const int n = toSockaddr(to, ss);
    s_ = fromFd(f);
    if (::connect(f, reinterpret_cast<const sockaddr*>(&ss), n) == 0) { connecting_ = false; return true; }
    if (isInProgress(lastError())) { connecting_ = true; return true; }
    close();
    return false;
}

int TcpSocket::connectStatus() {
    if (s_ == -1) return -1;
    if (!connecting_) return 1;
    if (!waitSocket(s_, true, 0)) return 0;
    int err = 0;
#if defined(_WIN32)
    int len = sizeof(err);
#else
    socklen_t len = sizeof(err);
#endif
    if (getsockopt(toFd(s_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0 || err != 0) { close(); return -1; }
    connecting_ = false;
    return 1;
}

void TcpSocket::close() {
    if (s_ != -1) closeFd(toFd(s_));
    s_ = -1;
    connecting_ = false;
}

Address TcpSocket::localAddress() const {
    Address a;
    if (s_ == -1) return a;
    sockaddr_storage ss{};
#if defined(_WIN32)
    int len = sizeof(ss);
#else
    socklen_t len = sizeof(ss);
#endif
    if (getsockname(toFd(s_), reinterpret_cast<sockaddr*>(&ss), &len) == 0) fromSockaddr(ss, a);
    return a;
}

Address TcpSocket::peerAddress() const {
    Address a;
    if (s_ == -1) return a;
    sockaddr_storage ss{};
#if defined(_WIN32)
    int len = sizeof(ss);
#else
    socklen_t len = sizeof(ss);
#endif
    if (getpeername(toFd(s_), reinterpret_cast<sockaddr*>(&ss), &len) == 0) fromSockaddr(ss, a);
    return a;
}

bool TcpSocket::setNoDelay(bool on) {
    int v = on ? 1 : 0;
    return s_ != -1 && setsockopt(toFd(s_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
}

int TcpSocket::send(const u8* data, usize len) {
    if (s_ == -1) return kSockError;
    int flags = 0;
#if defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;
#endif
    const auto r = ::send(toFd(s_), reinterpret_cast<const char*>(data), static_cast<int>(len), flags);
    if (r >= 0) return static_cast<int>(r);
    return isWouldBlock(lastError()) ? kWouldBlock : kSockError;
}

int TcpSocket::recv(u8* buf, usize cap) {
    if (s_ == -1) return kSockError;
    const auto r = ::recv(toFd(s_), reinterpret_cast<char*>(buf), static_cast<int>(cap), 0);
    if (r > 0) return static_cast<int>(r);
    if (r == 0) return cap == 0 ? kWouldBlock : kSockClosed;
    return isWouldBlock(lastError()) ? kWouldBlock : kSockError;
}

// ---- listener ----

bool TcpListener::listen(const Address& a, int backlog) {
    init();
    close();
    if (!a.valid()) return false;
    Fd f = ::socket(a.family == 4 ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (f == kBadFd) return false;
    if (!setNonBlocking(f)) { closeFd(f); return false; }
    if (a.family == 6) { int on = 1; setsockopt(f, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&on), sizeof(on)); }
#if !defined(_WIN32)
    { int on = 1; setsockopt(f, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on)); }
#endif
    sockaddr_storage ss;
    const int n = toSockaddr(a, ss);
    if (::bind(f, reinterpret_cast<const sockaddr*>(&ss), n) != 0 || ::listen(f, backlog) != 0) { closeFd(f); return false; }
    s_ = fromFd(f);
    return true;
}

bool TcpListener::accept(TcpSocket& out, Address& from) {
    if (s_ == -1) return false;
    sockaddr_storage ss{};
#if defined(_WIN32)
    int len = sizeof(ss);
#else
    socklen_t len = sizeof(ss);
#endif
    Fd f = ::accept(toFd(s_), reinterpret_cast<sockaddr*>(&ss), &len);
    if (f == kBadFd) return false;
    if (!setNonBlocking(f)) { closeFd(f); return false; }
    noSigpipe(f);
    out.close();
    out.s_ = fromFd(f);
    out.connecting_ = false;
    fromSockaddr(ss, from);
    return true;
}

void TcpListener::close() {
    if (s_ != -1) closeFd(toFd(s_));
    s_ = -1;
}

Address TcpListener::localAddress() const {
    Address a;
    if (s_ == -1) return a;
    sockaddr_storage ss{};
#if defined(_WIN32)
    int len = sizeof(ss);
#else
    socklen_t len = sizeof(ss);
#endif
    if (getsockname(toFd(s_), reinterpret_cast<sockaddr*>(&ss), &len) == 0) fromSockaddr(ss, a);
    return a;
}

} // namespace aver::net
