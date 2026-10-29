#include "aver/net/Address.hpp"
#include "SockPlatform.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace aver::net {

namespace detail {

void init() {
#if defined(_WIN32)
    static const bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    (void)ok;
#endif
}

int toSockaddr(const Address& a, sockaddr_storage& ss) {
    std::memset(&ss, 0, sizeof(ss));
    if (a.family == 4) {
        auto* s = reinterpret_cast<sockaddr_in*>(&ss);
        s->sin_family = AF_INET;
        s->sin_port = htons(a.port);
        std::memcpy(&s->sin_addr, a.ip, 4);
        return static_cast<int>(sizeof(sockaddr_in));
    }
    if (a.family == 6) {
        auto* s = reinterpret_cast<sockaddr_in6*>(&ss);
        s->sin6_family = AF_INET6;
        s->sin6_port = htons(a.port);
        std::memcpy(&s->sin6_addr, a.ip, 16);
        s->sin6_scope_id = a.scope;
        return static_cast<int>(sizeof(sockaddr_in6));
    }
    return 0;
}

bool fromSockaddr(const sockaddr_storage& ss, Address& a) {
    a = Address{};
    if (ss.ss_family == AF_INET) {
        const auto* s = reinterpret_cast<const sockaddr_in*>(&ss);
        a.family = 4;
        std::memcpy(a.ip, &s->sin_addr, 4);
        a.port = ntohs(s->sin_port);
        return true;
    }
    if (ss.ss_family == AF_INET6) {
        const auto* s = reinterpret_cast<const sockaddr_in6*>(&ss);
        a.family = 6;
        std::memcpy(a.ip, &s->sin6_addr, 16);
        a.port = ntohs(s->sin6_port);
        a.scope = s->sin6_scope_id;
        return true;
    }
    return false;
}

} // namespace detail

Address Address::v4(u8 a, u8 b, u8 c, u8 d, u16 port) {
    Address r;
    r.family = 4; r.ip[0] = a; r.ip[1] = b; r.ip[2] = c; r.ip[3] = d; r.port = port;
    return r;
}

Address Address::loopback6(u16 port) {
    Address r;
    r.family = 6; r.ip[15] = 1; r.port = port;
    return r;
}

Address Address::any6(u16 port) {
    Address r;
    r.family = 6; r.port = port;
    return r;
}

bool Address::parse(std::string_view text, u16 port, Address& out) {
    detail::init();
    std::string s(text);
    if (s.empty() || s.size() > 80) return false;
    if (s.front() == '[' && s.back() == ']') s = s.substr(1, s.size() - 2);
    out = Address{};
    out.port = port;
    unsigned scope = 0;
    if (const auto pct = s.find('%'); pct != std::string::npos) {
        const std::string z = s.substr(pct + 1);
        s.erase(pct);
        if (!z.empty() && std::all_of(z.begin(), z.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            unsigned long long v = 0;
            for (const char c : z) {
                v = v * 10 + static_cast<unsigned>(c - '0');
                if (v > 0xFFFFFFFFull) { out = Address{}; return false; }   // not a zone index
            }
            scope = static_cast<unsigned>(v);
        }
    }
    in_addr a4{};
    if (s.find(':') == std::string::npos && inet_pton(AF_INET, s.c_str(), &a4) == 1) {
        out.family = 4;
        std::memcpy(out.ip, &a4, 4);
        return true;
    }
    in6_addr a6{};
    if (s.find(':') != std::string::npos && inet_pton(AF_INET6, s.c_str(), &a6) == 1) {
        out.family = 6;
        std::memcpy(out.ip, &a6, 16);
        out.scope = scope;
        return true;
    }
    out = Address{};
    return false;
}

bool Address::isLoopback() const {
    if (family == 4) return ip[0] == 127;
    if (family == 6) {
        for (int i = 0; i < 15; ++i) if (ip[i] != 0) return false;
        return ip[15] == 1;
    }
    return false;
}

bool Address::isUnspecified() const {
    if (family != 4 && family != 6) return true;
    const int n = family == 4 ? 4 : 16;
    for (int i = 0; i < n; ++i) if (ip[i] != 0) return false;
    return true;
}

std::string Address::ipString() const {
    detail::init();
    char buf[INET6_ADDRSTRLEN + 16] = {};
    if (family == 4) {
        in_addr a{};
        std::memcpy(&a, ip, 4);
        inet_ntop(AF_INET, &a, buf, sizeof(buf));
    } else if (family == 6) {
        in6_addr a{};
        std::memcpy(&a, ip, 16);
        inet_ntop(AF_INET6, &a, buf, sizeof(buf));
        if (scope != 0) { const size_t n = std::strlen(buf); std::snprintf(buf + n, sizeof(buf) - n, "%%%u", scope); }
    }
    return buf;
}

std::string Address::toString() const {
    if (family == 6) return "[" + ipString() + "]:" + std::to_string(port);
    return ipString() + ":" + std::to_string(port);
}

bool operator==(const Address& a, const Address& b) {
    if (a.family != b.family || a.port != b.port) return false;
    if (a.family == 4) return std::memcmp(a.ip, b.ip, 4) == 0;
    if (a.family == 6) return std::memcmp(a.ip, b.ip, 16) == 0 && a.scope == b.scope;
    return true;
}

bool operator<(const Address& a, const Address& b) {
    if (a.family != b.family) return a.family < b.family;
    if (const int c = std::memcmp(a.ip, b.ip, 16); c != 0) return c < 0;
    if (a.port != b.port) return a.port < b.port;
    return a.scope < b.scope;
}

bool resolve(std::string_view host, int family, std::vector<Address>& out) {
    detail::init();
    out.clear();
    Address a;
    if (Address::parse(host, 0, a)) {
        if (family == 0 || a.family == family) out.push_back(a);
        return !out.empty();
    }
    const std::string name(host);
    if (name.empty() || name == "*") return false;
    addrinfo hints{};
    hints.ai_family = family == 4 ? AF_INET : family == 6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    for (addrinfo* p = res; p; p = p->ai_next) {
        sockaddr_storage ss{};
        std::memcpy(&ss, p->ai_addr, std::min<size_t>(static_cast<size_t>(p->ai_addrlen), sizeof(ss)));
        Address r;
        if (detail::fromSockaddr(ss, r) && std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
    }
    freeaddrinfo(res);
    return !out.empty();
}

std::vector<std::string> localAddresses() {
    detail::init();
    std::vector<std::string> out;
    auto add = [&](const Address& a) {
        if (!a.valid()) return;
        const std::string s = a.ipString();
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    };
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc == NO_ERROR) {
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                sockaddr_storage ss{};
                std::memcpy(&ss, u->Address.lpSockaddr, std::min<size_t>(static_cast<size_t>(u->Address.iSockaddrLength), sizeof(ss)));
                Address r;
                if (detail::fromSockaddr(ss, r)) add(r);
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* p = list; p; p = p->ifa_next) {
            if (!p->ifa_addr || !(p->ifa_flags & IFF_UP)) continue;
            sockaddr_storage ss{};
            const size_t n = p->ifa_addr->sa_family == AF_INET ? sizeof(sockaddr_in) : p->ifa_addr->sa_family == AF_INET6 ? sizeof(sockaddr_in6) : 0;
            if (n == 0) continue;
            std::memcpy(&ss, p->ifa_addr, n);
            Address r;
            if (detail::fromSockaddr(ss, r)) add(r);
        }
        freeifaddrs(list);
    }
#endif
    if (out.empty()) { add(Address::loopback4()); add(Address::loopback6()); }
    return out;
}

} // namespace aver::net
