#pragma once
// A network endpoint (IPv4 or IPv6 plus a port), numeric parsing, name resolution and the local interface addresses.
// G6 NET1 (https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G6_NETWORK.md).
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::net {

struct Address {
    u8  family = 0;          // 0 unset, 4, 6
    u8  ip[16] = {};         // IPv4 uses the first four bytes
    u16 port = 0;
    u32 scope = 0;           // IPv6 scope id

    static Address v4(u8 a, u8 b, u8 c, u8 d, u16 port = 0);
    static Address loopback4(u16 port = 0) { return v4(127, 0, 0, 1, port); }
    static Address loopback6(u16 port = 0);
    static Address any4(u16 port = 0) { return v4(0, 0, 0, 0, port); }
    static Address any6(u16 port = 0);

    // Numeric text only ("127.0.0.1", "::1", "fe80::1%3"); false for a name. "*" is not accepted here.
    static bool parse(std::string_view text, u16 port, Address& out);

    bool valid() const { return family == 4 || family == 6; }
    bool isLoopback() const;
    bool isUnspecified() const;
    std::string ipString() const;
    std::string toString() const;   // "ip:port" or "[ip]:port"

    friend bool operator==(const Address& a, const Address& b);
    friend bool operator!=(const Address& a, const Address& b) { return !(a == b); }
    friend bool operator<(const Address& a, const Address& b);
};

// Numeric text or a host name. family 0 = any, 4, 6. Blocks on the system resolver for a name; the numeric case never does.
bool resolve(std::string_view host, int family, std::vector<Address>& out);

// Every address of every interface that is up (the local addresses), numeric text, loopback included.
std::vector<std::string> localAddresses();

} // namespace aver::net
