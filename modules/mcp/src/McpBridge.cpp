// The MCP control channel: the line protocol parser, the loopback listener, and the frame pump.
#include "aver/mcp/McpBridge.hpp"

#include "aver/core/Log.hpp"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

namespace aver::mcp {
namespace {

// Reads an integer value for a flat JSON key. Returns false when the key is absent or not a number.
bool findNumber(const std::string& s, const char* key, i64& out) {
    const std::string pat = std::string("\"") + key + "\"";
    const usize k = s.find(pat);
    if (k == std::string::npos) return false;
    usize c = s.find(':', k + pat.size());
    if (c == std::string::npos) return false;
    ++c;
    while (c < s.size() && std::isspace(static_cast<unsigned char>(s[c]))) ++c;
    const usize start = c;
    if (c < s.size() && (s[c] == '-' || s[c] == '+')) ++c;
    while (c < s.size() && std::isdigit(static_cast<unsigned char>(s[c]))) ++c;
    if (c == start) return false;
    out = std::strtoll(s.substr(start, c - start).c_str(), nullptr, 10);
    return true;
}

// Reads four hex digits at s[at..at+4). False when any is not a hex digit or the string is too short.
bool hex4(const std::string& s, usize at, u32& out) {
    if (at + 4 > s.size()) return false;
    u32 v = 0;
    for (usize i = 0; i < 4; ++i) {
        const char h = s[at + i];
        u32 d;
        if (h >= '0' && h <= '9') d = static_cast<u32>(h - '0');
        else if (h >= 'a' && h <= 'f') d = static_cast<u32>(h - 'a') + 10u;
        else if (h >= 'A' && h <= 'F') d = static_cast<u32>(h - 'A') + 10u;
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

// Appends one code point as UTF-8.
void appendUtf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Reads a string value for a flat JSON key, undoing every JSON escape. Returns false when the key is
// absent or the string is unterminated. \n \r \t \b \f and \uXXXX (surrogate pairs included) used to
// come back as a bare letter, so a multi-line "text" arrived as one run-together line.
bool findString(const std::string& s, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\"";
    const usize k = s.find(pat);
    if (k == std::string::npos) return false;
    usize c = s.find(':', k + pat.size());
    if (c == std::string::npos) return false;
    c = s.find('"', c);
    if (c == std::string::npos) return false;
    ++c;
    std::string v;
    while (c < s.size() && s[c] != '"') {
        if (s[c] != '\\' || c + 1 >= s.size()) { v.push_back(s[c++]); continue; }
        ++c;                                      // on the escape letter
        switch (s[c]) {
            case 'n': v.push_back('\n'); ++c; continue;
            case 'r': v.push_back('\r'); ++c; continue;
            case 't': v.push_back('\t'); ++c; continue;
            case 'b': v.push_back('\b'); ++c; continue;
            case 'f': v.push_back('\f'); ++c; continue;
            case 'u': {
                u32 cp = 0;
                if (!hex4(s, c + 1, cp)) break;   // not a valid escape: keep the letter, as before
                usize next = c + 5;
                // A high surrogate pairs with the \uXXXX that must follow it.
                u32 lo = 0;
                if (cp >= 0xD800 && cp <= 0xDBFF && next + 1 < s.size() && s[next] == '\\' &&
                    s[next + 1] == 'u' && hex4(s, next + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    next += 6;
                }
                appendUtf8(v, cp);
                c = next;
                continue;
            }
            default: break;                       // \" \\ \/ : the escaped character itself
        }
        v.push_back(s[c++]);
    }
    if (c >= s.size()) return false;
    out = v;
    return true;
}

// The virtual-key code for a key name. Returns 0 for an unknown name.
u32 keyCodeFor(const std::string& name) {
    if (name.size() == 1) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<u32>(c);
    }
    struct Named { const char* n; u32 vk; };
    static const Named table[] = {
        {"escape", 0x1B}, {"esc", 0x1B}, {"enter", 0x0D}, {"return", 0x0D}, {"tab", 0x09},
        {"space", 0x20}, {"delete", 0x2E}, {"backspace", 0x08},
        {"left", 0x25}, {"up", 0x26}, {"right", 0x27}, {"down", 0x28},
        {"f1", 0x70}, {"f2", 0x71}, {"f3", 0x72}, {"f4", 0x73}, {"f5", 0x74}, {"f6", 0x75},
        {"ctrl", 0x11}, {"shift", 0x10}, {"alt", 0x12},
    };
    std::string lower;
    for (char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (const Named& e : table) if (lower == e.n) return e.vk;
    return 0;
}

// Escapes a value being placed inside a JSON string.
std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c) & 0xFFu);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// The button index for a button name. Defaults to left.
int buttonFor(const std::string& s) {
    if (s == "right") return 1;
    if (s == "middle") return 2;
    return 0;
}

// How long an abi request waits for the main thread. A request that times out still runs later, so
// a short limit reports failure for a call that goes on to succeed (a batch place builds collision).
constexpr std::chrono::seconds kAbiWait{60};

// The wait above is taken in slices this long, so it can end early when the client hangs up or the bridge
// is stopping. A notify wakes it at once; the slice only bounds how late those two are noticed.
constexpr std::chrono::milliseconds kAbiSlice{100};

#if defined(_WIN32)
// True when the client has closed or reset its end. A zero-timeout select says whether the socket is
// readable at all; a one-byte MSG_PEEK then tells data (the client pipelined its next line, so it is still
// there) from an orderly close (0 bytes) or a reset (an error). Consumes nothing.
bool clientHungUp(SOCKET s) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    timeval none{0, 0};
    if (::select(0, &readable, nullptr, nullptr, &none) <= 0) return false;
    char byte = 0;
    const int got = ::recv(s, &byte, 1, MSG_PEEK);
    return got == 0 || (got == SOCKET_ERROR && ::WSAGetLastError() != WSAEWOULDBLOCK);
}
#endif

} // namespace

// Parses one protocol line into a Command, expanding a click into move/down/up.
bool parseCommand(const std::string& line, Command& out, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };

    out = Command{};
    std::string cmd;
    if (!findString(line, "cmd", cmd)) return fail("no \"cmd\" field");
    out.name = cmd;
    i64 v = 0;
    if (findNumber(line, "id", v)) out.id = static_cast<u64>(v);

    i64 x = 0, y = 0;
    const bool haveXY = findNumber(line, "x", x) && findNumber(line, "y", y);

    if (cmd == "ping") return true;

    // Answered on the socket thread by the worker loop, not queued.
    if (cmd == "modules") return true;
    if (cmd == "widgets") return true;

    if (cmd == "shot") {
        if (!findString(line, "path", out.arg)) return fail("shot needs a \"path\"");
        return true;
    }

    if (cmd == "move") {
        if (!haveXY) return fail("move needs \"x\" and \"y\"");
        InputEvent e; e.kind = InputEvent::Kind::MouseMove;
        e.x = static_cast<i32>(x); e.y = static_cast<i32>(y);
        out.events.push_back(e);
        return true;
    }

    if (cmd == "click") {
        std::string widget;
        if (findString(line, "widget", widget)) {
            out.arg = widget;
            std::string b; findString(line, "button", b);
            // The button is stashed on an otherwise-empty event; the worker loop fills in the point.
            InputEvent pending; pending.kind = InputEvent::Kind::MouseDown;
            pending.button = buttonFor(b);
            out.events.push_back(pending);
            return true;
        }
        if (!haveXY) return fail("click needs \"x\" and \"y\", or a \"widget\" name");
        std::string b; findString(line, "button", b);
        const int btn = buttonFor(b);
        InputEvent mv; mv.kind = InputEvent::Kind::MouseMove;
        mv.x = static_cast<i32>(x); mv.y = static_cast<i32>(y);
        InputEvent dn = mv; dn.kind = InputEvent::Kind::MouseDown; dn.button = btn;
        InputEvent up = mv; up.kind = InputEvent::Kind::MouseUp;   up.button = btn;
        out.events.push_back(mv);
        out.events.push_back(dn);
        out.events.push_back(up);
        return true;
    }

    if (cmd == "key") {
        std::string k;
        if (!findString(line, "key", k)) return fail("key needs a \"key\"");
        const u32 vk = keyCodeFor(k);
        if (!vk) return fail("unknown key name");
        InputEvent dn; dn.kind = InputEvent::Kind::KeyDown; dn.key = vk;
        InputEvent up; up.kind = InputEvent::Kind::KeyUp;   up.key = vk;
        out.events.push_back(dn);
        out.events.push_back(up);
        return true;
    }

    if (cmd == "text") {
        std::string t;
        if (!findString(line, "text", t)) return fail("text needs a \"text\"");
        InputEvent e; e.kind = InputEvent::Kind::Text; e.text = t;
        out.events.push_back(e);
        return true;
    }

    if (cmd == "abi") {
        if (!findString(line, "module", out.abi.module)) return fail("abi needs a \"module\"");
        if (!findString(line, "fn", out.abi.fn)) return fail("abi needs an \"fn\"");
        findString(line, "text", out.abi.text);
        const usize a = line.find("\"args\"");
        if (a != std::string::npos) {
            const usize lb = line.find('[', a);
            const usize rb = lb == std::string::npos ? std::string::npos : line.find(']', lb);
            if (lb == std::string::npos || rb == std::string::npos) return fail("abi args must be an array");
            usize c = lb + 1;
            while (c < rb) {
                while (c < rb && (std::isspace(static_cast<unsigned char>(line[c])) || line[c] == ',')) ++c;
                if (c >= rb) break;
                const usize start = c;
                if (line[c] == '-' || line[c] == '+') ++c;
                while (c < rb && (std::isdigit(static_cast<unsigned char>(line[c])) ||
                                  line[c] == '.' || line[c] == 'e' || line[c] == 'E' ||
                                  line[c] == '-' || line[c] == '+')) ++c;
                if (c == start) return fail("abi args holds something that is not a number");
                out.abi.args.push_back(std::strtod(line.substr(start, c - start).c_str(), nullptr));
            }
        }
        return true;
    }

    return fail("unknown cmd");
}

// The bridge's state: the listener thread, the command queue and the ABI registry.
struct McpBridge::Impl {
    std::thread worker;
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::deque<Command> queue;
    usize cursor = 0;   // how far into queue.front()'s events pump() has got
    u16 port = 0;
    std::map<std::string, AbiDispatch> abis;   // module name -> that module's ABI
    WidgetResolver resolver;
    WidgetLister lister;
#if defined(_WIN32)
    SOCKET listener = INVALID_SOCKET;
    // The ACCEPTED connection, published so stop() can reach it. Closing the listener only unblocks
    // accept(); a worker already inside recv() on a live client never noticed, and stop()'s join()
    // then waited for the client to disconnect on its own -- an editor that would not close while an
    // MCP client held the socket open. Atomic because stop() runs on the main thread.
    std::atomic<SOCKET> client{INVALID_SOCKET};
    bool wsaUp = false;
#endif
};

// Allocates the bridge's state. Nothing listens until start().
McpBridge::McpBridge() : impl_(std::make_unique<Impl>()) {}
// Stops the channel.
McpBridge::~McpBridge() { stop(); }

bool McpBridge::listening() const { return impl_ && impl_->running.load(); }
u16  McpBridge::port() const { return impl_ ? impl_->port : 0; }

// Registers, or replaces, the dispatcher a module's calls are routed to.
void McpBridge::registerAbi(const std::string& module, AbiDispatch dispatch) {
    if (!impl_ || module.empty() || !dispatch) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const bool replacing = impl_->abis.find(module) != impl_->abis.end();
    impl_->abis[module] = std::move(dispatch);
    AVER_INFO("[Mcp] ABI registered: {}{}", module, replacing ? " (replacing)" : "");
}

// Installs the resolver that turns a widget name into a point.
void McpBridge::setWidgetResolver(WidgetResolver fn) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->resolver = std::move(fn);
}

// Installs the lister that answers a widgets request.
void McpBridge::setWidgetLister(WidgetLister fn) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->lister = std::move(fn);
}

// Routes a call to its module's dispatcher. Refuses, naming the module, when none is registered.
bool McpBridge::callAbi(const AbiCall& call, std::string& result, std::string& why) const {
    AbiDispatch fn;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto it = impl_->abis.find(call.module);
        if (it == impl_->abis.end()) {
            why = "no ABI registered for '" + call.module +
                  "' -- either the name is wrong or that module was not built into this binary";
            return false;
        }
        fn = it->second;
    }
    // Called outside the lock: a dispatcher runs module code of unknown duration.
    return fn(call, result, why);
}

// Every module name with an ABI registered, sorted.
std::vector<std::string> McpBridge::modules() const {
    std::vector<std::string> out;
    if (!impl_) return out;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& kv : impl_->abis) out.push_back(kv.first);   // std::map: already sorted
    return out;
}

#if !defined(_WIN32)
// Refuses to start: the control channel is Windows-only.
bool McpBridge::start(u16) {
    AVER_WARN("[Mcp] the control channel is Windows-only for now; not started");
    return false;
}
// Nothing to stop off Windows.
void McpBridge::stop() {}
// Nothing to pump off Windows.
u32 McpBridge::pump(const std::function<void(const Command&)>&) { return 0; }
#else

// Binds the loopback listener and starts the socket thread. Returns false and logs why on failure.
bool McpBridge::start(u16 port) {
    if (impl_->running.load()) return true;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        AVER_ERROR("[Mcp] WSAStartup failed; the control channel is not available");
        return false;
    }
    impl_->wsaUp = true;

    impl_->listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listener == INVALID_SOCKET) {
        AVER_ERROR("[Mcp] could not create a socket");
        WSACleanup(); impl_->wsaUp = false;
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    // Loopback, hard-coded: this must never be reachable from another machine.
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::bind(impl_->listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR ||
        ::listen(impl_->listener, 1) == SOCKET_ERROR) {
        AVER_ERROR("[Mcp] could not listen on 127.0.0.1:{} (is another editor already using it?)", port);
        ::closesocket(impl_->listener);
        impl_->listener = INVALID_SOCKET;
        WSACleanup(); impl_->wsaUp = false;
        return false;
    }

    impl_->port = port;
    impl_->running.store(true);
    Impl* impl = impl_.get();
    impl_->worker = std::thread([impl] {
        while (impl->running.load()) {
            const SOCKET client = ::accept(impl->listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;          // closed by stop()
            impl->client.store(client, std::memory_order_release);
            std::string buffer;
            char chunk[1024];
            bool drop = false;   // the connection is over (hung up, or the bridge is stopping): send nothing more
            while (impl->running.load()) {
                const int got = ::recv(client, chunk, sizeof chunk, 0);
                if (got <= 0) break;
                buffer.append(chunk, static_cast<usize>(got));
                usize nl;
                while ((nl = buffer.find('\n')) != std::string::npos) {
                    const std::string line = buffer.substr(0, nl);
                    buffer.erase(0, nl + 1);
                    Command c;
                    std::string why;
                    std::string reply;
                    // A click by name is resolved here, on the socket thread.
                    if (parseCommand(line, c, &why) && c.name == "click" && !c.arg.empty()) {
                        WidgetResolver resolve;
                        {
                            std::lock_guard<std::mutex> lock(impl->mutex);
                            resolve = impl->resolver;
                        }
                        f32 wx = 0.0f, wy = 0.0f;
                        const int btn = c.events.empty() ? 0 : c.events[0].button;
                        if (!resolve) {
                            reply = "{\"id\":" + std::to_string(c.id) +
                                    ",\"ok\":false,\"error\":\"this build resolves no widget names\"}\n";
                        } else if (!resolve(c.arg, wx, wy)) {
                            reply = "{\"id\":" + std::to_string(c.id) +
                                    ",\"ok\":false,\"error\":\"no widget named " + jsonEscape(c.arg) +
                                    "; ask cmd=widgets for the list\"}\n";
                        } else {
                            // Expanded into exactly what a coordinate click produces.
                            Command k;
                            k.name = "click";
                            k.id = c.id;
                            InputEvent mv; mv.kind = InputEvent::Kind::MouseMove;
                            mv.x = static_cast<i32>(wx); mv.y = static_cast<i32>(wy);
                            InputEvent dn = mv; dn.kind = InputEvent::Kind::MouseDown; dn.button = btn;
                            InputEvent up = mv; up.kind = InputEvent::Kind::MouseUp;   up.button = btn;
                            k.events.push_back(mv); k.events.push_back(dn); k.events.push_back(up);
                            {
                                std::lock_guard<std::mutex> lock(impl->mutex);
                                impl->queue.push_back(k);
                            }
                            // The resolved point comes back, so a client can see where it clicked.
                            reply = "{\"id\":" + std::to_string(c.id) +
                                    ",\"ok\":true,\"at\":[" + std::to_string((int)wx) +
                                    "," + std::to_string((int)wy) + "]}\n";
                        }
                    } else if (parseCommand(line, c, &why) && c.name == "widgets") {
                        WidgetLister list;
                        {
                            std::lock_guard<std::mutex> lock(impl->mutex);
                            list = impl->lister;
                        }
                        reply = "{\"id\":" + std::to_string(c.id) + ",\"ok\":true,\"widgets\":\"" +
                                jsonEscape(list ? list() : std::string()) + "\"}\n";
                    } else if (parseCommand(line, c, &why) && c.name == "modules") {
                        std::string list;
                        {
                            std::lock_guard<std::mutex> lock(impl->mutex);
                            for (const auto& kv : impl->abis) {
                                if (!list.empty()) list += ",";
                                list += "\"" + kv.first + "\"";
                            }
                        }
                        reply = "{\"id\":" + std::to_string(c.id) +
                                ",\"ok\":true,\"modules\":[" + list + "]}\n";
                    } else if (parseCommand(line, c, &why) && c.name == "abi") {
                        // Waited on: the reply carries the call's real outcome.
                        c.pending = std::make_shared<PendingResult>();
                        {
                            std::lock_guard<std::mutex> lock(impl->mutex);
                            impl->queue.push_back(c);
                        }
                        // In short slices rather than one 60 s wait, so it can end for the two reasons
                        // that are not an answer. The client hung up: the bridge serves one connection,
                        // and holding it for the rest of the minute served nobody. Or stop() ran: it
                        // drains the queue once, and a call pushed just after would sit out the whole
                        // timeout with join() waiting on this thread.
                        bool answered = false, stopping = false, hungUp = false;
                        {
                            std::unique_lock<std::mutex> wait(c.pending->mutex);
                            const auto deadline = std::chrono::steady_clock::now() + kAbiWait;
                            for (;;) {
                                if (c.pending->done) { answered = true; break; }
                                stopping = !impl->running.load() ||
                                           impl->client.load(std::memory_order_acquire) != client;
                                if (stopping) break;
                                hungUp = clientHungUp(client);
                                if (hungUp || std::chrono::steady_clock::now() >= deadline) break;
                                c.pending->cv.wait_for(wait, kAbiSlice);
                            }
                        }
                        if (stopping || hungUp) {
                            // The call is already queued and still runs; its answer has nowhere to go.
                            if (hungUp)
                                AVER_WARN("[Mcp] the client hung up while abi {}::{} was pending; the call "
                                          "still runs, and its answer is dropped", c.abi.module, c.abi.fn);
                            drop = true;
                        } else if (!answered) {
                            reply = "{\"id\":" + std::to_string(c.id) +
                                    ",\"ok\":false,\"error\":\"timed out after " +
                                    std::to_string(kAbiWait.count()) + "s -- the editor did not "
                                    "pump; is it running and not shutting down?\"}\n";
                        } else if (c.pending->ok) {
                            reply = "{\"id\":" + std::to_string(c.id) + ",\"ok\":true,\"result\":\"" +
                                    jsonEscape(c.pending->result) + "\"}\n";
                        } else {
                            reply = "{\"id\":" + std::to_string(c.id) + ",\"ok\":false,\"error\":\"" +
                                    jsonEscape(c.pending->why) + "\"}\n";
                        }
                    } else if (parseCommand(line, c, &why)) {
                        {
                            std::lock_guard<std::mutex> lock(impl->mutex);
                            impl->queue.push_back(c);
                        }
                        // Input commands stay fire-and-forget.
                        reply = "{\"id\":" + std::to_string(c.id) + ",\"ok\":true}\n";
                    } else {
                        reply = "{\"id\":" + std::to_string(c.id) + ",\"ok\":false,\"error\":\"" +
                                why + "\"}\n";
                    }
                    // stop() closes whatever socket it finds in `client`; past that the handle value may
                    // already belong to someone else, so nothing more is sent on it.
                    if (!drop && impl->client.load(std::memory_order_acquire) != client) drop = true;
                    if (drop) break;
                    ::send(client, reply.c_str(), static_cast<int>(reply.size()), 0);
                }
                if (drop) break;
            }
            // Taken back before closing, so stop() cannot close the same socket a second time.
            if (impl->client.exchange(INVALID_SOCKET, std::memory_order_acq_rel) != INVALID_SOCKET)
                ::closesocket(client);
        }
    });

    AVER_INFO("[Mcp] control channel listening on 127.0.0.1:{} -- the editor does not need it, and it "
              "does nothing until a client connects", port);
    return true;
}

// Closes the socket, releases every waiting caller and joins the worker thread.
void McpBridge::stop() {
    if (!impl_ || !impl_->running.load()) return;
    impl_->running.store(false);
    if (impl_->listener != INVALID_SOCKET) {
        // Closed before joining, so the blocking accept() returns.
        ::closesocket(impl_->listener);
        impl_->listener = INVALID_SOCKET;
    }
    // And the live connection, so a worker blocked in recv() returns too. Without this the join
    // below waited on a thread that would not wake until the client chose to disconnect.
    {
        const SOCKET c = impl_->client.exchange(INVALID_SOCKET, std::memory_order_acq_rel);
        if (c != INVALID_SOCKET) ::closesocket(c);
    }
    // Fails every queued call and empties the queue. Run twice: before the join, so waiting callers are
    // released at once, and after it, because the worker can push one more call between the first drain and
    // its own exit (it re-checks `running` only every slice) -- left queued it would run against a
    // channel that has been stopped, or after a later start().
    const auto drainQueue = [this] {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (Command& q : impl_->queue) {
            if (!q.pending) continue;
            {
                std::lock_guard<std::mutex> p(q.pending->mutex);
                q.pending->ok = false;
                q.pending->why = "the editor is shutting down";
                q.pending->done = true;
            }
            q.pending->cv.notify_all();
        }
        impl_->queue.clear();
        impl_->cursor = 0;
    };
    drainQueue();
    // The join is now bounded by one wait slice, not the 60 s ABI timeout: the worker's wait re-checks
    // `running` and the connection every kAbiSlice.
    if (impl_->worker.joinable()) impl_->worker.join();
    drainQueue();
    if (impl_->wsaUp) { WSACleanup(); impl_->wsaUp = false; }
    AVER_INFO("[Mcp] control channel stopped");
}

// Delivers ONE queued input event to `apply`, so a click straddles frames the way ImGui needs.
// Returns how many commands were applied.
u32 McpBridge::pump(const std::function<void(const Command&)>& apply) {
    if (!impl_ || !apply) return 0;

    Command single;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->queue.empty()) return 0;
        Command& front = impl_->queue.front();

        // A command with no input events at all -- ping, shot -- is delivered whole and retired.
        if (front.events.empty()) {
            single = front;
            impl_->queue.pop_front();
            impl_->cursor = 0;
        } else {
            single.name = front.name;
            single.arg  = front.arg;
            single.id   = front.id;
            single.events.push_back(front.events[impl_->cursor]);
            if (++impl_->cursor >= front.events.size()) {
                impl_->queue.pop_front();
                impl_->cursor = 0;
            }
        }
    }
    // An ABI call is dispatched here, on the main thread, and its waiter released.
    if (single.name == "abi") {
        std::string result, why;
        const bool ok = callAbi(single.abi, result, why);
        if (single.pending) {
            {
                std::lock_guard<std::mutex> lock(single.pending->mutex);
                single.pending->ok = ok;
                single.pending->result = result;
                single.pending->why = why;
                single.pending->done = true;
            }
            single.pending->cv.notify_all();
        }
        // NOT handed to apply() as well. The host's callback has its own `if (c.name == "abi")`
        // branch that calls callAbi on the SAME AbiCall, so every abi request ran its dispatcher
        // twice -- once here and once there -- while the reply the client already received
        // described only the first. A dispatcher with any side effect ran them both.
        //
        // The waiter has been released with this call's result above, so there is nothing left for
        // the host to do with it.
        return 1;
    }

    apply(single);
    return 1;
}
#endif // _WIN32

} // namespace aver::mcp
