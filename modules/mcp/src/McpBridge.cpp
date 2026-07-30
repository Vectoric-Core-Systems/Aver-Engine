// See McpBridge.hpp. In particular: the editor must work without this module, so nothing here is
// reachable unless AVER_MODULE_MCP built it and the app called start().
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

// A deliberately tiny scalar reader. This does NOT reach for Aver.Formats' JSON DOM: the wire protocol
// is a handful of flat keys, and taking a dependency on the formats module -- which pulls in Platform
// and Assets -- to read {"x":100} would make an optional debugging channel heavier than the thing it
// controls.
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

bool findString(const std::string& s, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\"";
    const usize k = s.find(pat);
    if (k == std::string::npos) return false;
    usize c = s.find(':', k + pat.size());
    if (c == std::string::npos) return false;
    c = s.find('"', c);
    if (c == std::string::npos) return false;
    const usize start = ++c;
    std::string v;
    while (c < s.size() && s[c] != '"') {
        if (s[c] == '\\' && c + 1 < s.size()) ++c;   // one level of escape, enough for a path
        v.push_back(s[c++]);
    }
    if (c >= s.size()) return false;
    out = v;
    return true;
}

// Virtual-key codes for the keys an editor test actually needs, by name. A table rather than a cast of
// the first character, because "F" and "F1" are different keys and a caller should be able to say
// either without knowing Win32's numbering.
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

// Escape a value being placed inside a JSON string.
//
// NOT OPTIONAL, and it was missing. A reply carrying a Windows path -- which `editor::screenshot`
// returns, and which is one of the most obvious things to ask for -- emitted
// "result":"C:\Users\..." and every client's JSON parser rejected the line. The reply was invalid
// from the first backslash, and the failure looked like the server had gone away rather than like a
// quoting bug.
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
                // Control characters are not legal raw inside a JSON string either.
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

int buttonFor(const std::string& s) {
    if (s == "right") return 1;
    if (s == "middle") return 2;
    return 0;   // left, and the default
}

} // namespace

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

    // "what can I reach?" -- the first thing a client should be able to ask, and the answer is this
    // binary's build configuration rather than a fixed list. Answered on the socket thread; see the
    // worker loop for why it is not queued.
    if (cmd == "modules") return true;
    // What can be clicked, by name. Answered on the socket thread like `modules`.
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
        // BY NAME, and this is the form a client should prefer. A coordinate click is only as good as
        // the coordinate, and reading one off a screenshot survives exactly until the layout moves or
        // the DPI changes. The name is resolved against what the editor actually drew -- see the
        // worker loop, which expands this into the same three events a coordinate click produces.
        std::string widget;
        if (findString(line, "widget", widget)) {
            out.arg = widget;
            std::string b; findString(line, "button", b);
            // The button is stashed on an otherwise-empty event so resolution can fill in the point
            // without re-parsing the line.
            InputEvent pending; pending.kind = InputEvent::Kind::MouseDown;
            pending.button = buttonFor(b);
            out.events.push_back(pending);
            return true;
        }
        if (!haveXY) return fail("click needs \"x\" and \"y\", or a \"widget\" name");
        std::string b; findString(line, "button", b);
        const int btn = buttonFor(b);
        // EXPANDED HERE, into move-then-down-then-up. A click is three events and the editor should not
        // have to know that; more importantly, ImGui only registers a press it saw in one frame and a
        // release it saw in another, so the pacing in pump() needs them as separate events rather than
        // as one opaque "click".
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
        // ROUTED, not interpreted. This module does not know what "framework" means -- it knows only
        // that something registered under that name, and hands the call there. See McpBridge::callAbi.
        if (!findString(line, "module", out.abi.module)) return fail("abi needs a \"module\"");
        if (!findString(line, "fn", out.abi.fn)) return fail("abi needs an \"fn\"");
        findString(line, "text", out.abi.text);
        // The numeric argument list. Scanned by hand for the same reason findNumber is: a flat array of
        // numbers does not justify pulling Aver.Formats -- and its Platform and Assets dependencies --
        // into an optional debugging channel.
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

    // REFUSED, not ignored. A client that misspelled `click` should be told, rather than left waiting
    // for a button that was never pressed.
    return fail("unknown cmd");
}

// --------------------------------------------------------------------------------------------------

struct McpBridge::Impl {
    std::thread worker;
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::deque<Command> queue;
    // How far into queue.front()'s events pump() has got. A CURSOR rather than popping the command,
    // because pacing has to be per EVENT: see pump().
    usize cursor = 0;
    u16 port = 0;
    // name -> that module's ABI. A map, not a switch: a switch would mean this module knew every
    // module's name, and knowing them is one step from linking them.
    std::map<std::string, AbiDispatch> abis;
    WidgetResolver resolver;
    WidgetLister lister;
#if defined(_WIN32)
    SOCKET listener = INVALID_SOCKET;
    bool wsaUp = false;
#endif
};

McpBridge::McpBridge() : impl_(std::make_unique<Impl>()) {}
McpBridge::~McpBridge() { stop(); }

bool McpBridge::listening() const { return impl_ && impl_->running.load(); }
u16  McpBridge::port() const { return impl_ ? impl_->port : 0; }

void McpBridge::registerAbi(const std::string& module, AbiDispatch dispatch) {
    if (!impl_ || module.empty() || !dispatch) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const bool replacing = impl_->abis.find(module) != impl_->abis.end();
    impl_->abis[module] = std::move(dispatch);
    // Said out loud, because the registry IS the reachable surface: a reader of the log should be able
    // to see exactly which module seams this binary exposes, without inferring it from the build flags.
    AVER_INFO("[Mcp] ABI registered: {}{}", module, replacing ? " (replacing)" : "");
}

void McpBridge::setWidgetResolver(WidgetResolver fn) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->resolver = std::move(fn);
}

void McpBridge::setWidgetLister(WidgetLister fn) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->lister = std::move(fn);
}

bool McpBridge::callAbi(const AbiCall& call, std::string& result, std::string& why) const {
    AbiDispatch fn;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto it = impl_->abis.find(call.module);
        if (it == impl_->abis.end()) {
            // Named, and with the likely cause. "no ABI for physics" sends a reader to look for a typo;
            // saying the module may simply not be in this build sends them to the switch that decides it.
            why = "no ABI registered for '" + call.module +
                  "' -- either the name is wrong or that module was not built into this binary";
            return false;
        }
        fn = it->second;
    }
    // Called OUTSIDE the lock. A dispatcher runs module code of unknown duration, and holding the queue
    // mutex across it would stall the socket thread for as long as the engine took to answer.
    return fn(call, result, why);
}

std::vector<std::string> McpBridge::modules() const {
    std::vector<std::string> out;
    if (!impl_) return out;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& kv : impl_->abis) out.push_back(kv.first);   // std::map: already sorted
    return out;
}

#if !defined(_WIN32)
bool McpBridge::start(u16) {
    AVER_WARN("[Mcp] the control channel is Windows-only for now; not started");
    return false;
}
void McpBridge::stop() {}
u32 McpBridge::pump(const std::function<void(const Command&)>&) { return 0; }
#else

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
    // LOOPBACK, hard-coded. This posts synthetic clicks into a running editor; it must not be
    // reachable from another machine, and a configurable address would be offering that as an option.
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
            std::string buffer;
            char chunk[1024];
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
                    // A click BY NAME is resolved here, on the socket thread, so an unknown name is
                    // refused instantly and specifically rather than a frame later and silently.
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
                            // NAMED and ACTIONABLE. "no widget named tool.rotor" plus where to get the
                            // real list is the single most useful thing a client can be told here, and
                            // it is useless a frame late -- which is why this is answered from the
                            // socket thread rather than queued.
                            reply = "{\"id\":" + std::to_string(c.id) +
                                    ",\"ok\":false,\"error\":\"no widget named " + jsonEscape(c.arg) +
                                    "; ask cmd=widgets for the list\"}\n";
                        } else {
                            // Expanded into exactly what a coordinate click produces, so there is ONE
                            // path from here on and pacing stays in pump().
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
                            // The resolved point comes BACK. A client that asked for a name can then
                            // check where it actually clicked, which is the difference between "it did
                            // nothing" and "it clicked the wrong thing".
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
                        // Answered HERE rather than queued. It reads the registry, which is
                        // lock-guarded and needs no frame at all, so queueing it would add a frame of
                        // latency to the one question a client asks before anything else.
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
                        // WAITED ON, because this is a question. The old code queued it and answered
                        // {"ok":true} straight away -- so a client was told its call had succeeded
                        // before anything had tried it, and `nosuch::x` came back ok:true while the
                        // refusal went only to the log. An acknowledgement that cannot say no is not one.
                        c.pending = std::make_shared<PendingResult>();
                        {
                            std::lock_guard<std::mutex> lock(impl->mutex);
                            impl->queue.push_back(c);
                        }
                        std::unique_lock<std::mutex> wait(c.pending->mutex);
                        // Bounded. If the editor stalls, is minimised into never pumping, or is shutting
                        // down, a client must get an answer rather than a socket that never speaks
                        // again -- and "timed out" is itself diagnostic.
                        const bool answered = c.pending->cv.wait_for(
                            wait, std::chrono::seconds(5), [&] { return c.pending->done; });
                        if (!answered) {
                            reply = "{\"id\":" + std::to_string(c.id) +
                                    ",\"ok\":false,\"error\":\"timed out after 5s -- the editor did not "
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
                        // Input commands stay fire-and-forget: a click has no return value, and making
                        // a client wait a frame for "yes, that was queued" would slow every gesture for
                        // no information.
                        reply = "{\"id\":" + std::to_string(c.id) + ",\"ok\":true}\n";
                    } else {
                        // Answered immediately rather than queued: a malformed command has nothing for
                        // the main thread to do, and a client should not wait a frame to learn it.
                        reply = "{\"id\":" + std::to_string(c.id) + ",\"ok\":false,\"error\":\"" +
                                why + "\"}\n";
                    }
                    ::send(client, reply.c_str(), static_cast<int>(reply.size()), 0);
                }
            }
            ::closesocket(client);
        }
    });

    AVER_INFO("[Mcp] control channel listening on 127.0.0.1:{} -- the editor does not need it, and it "
              "does nothing until a client connects", port);
    return true;
}

void McpBridge::stop() {
    if (!impl_ || !impl_->running.load()) return;
    impl_->running.store(false);
    if (impl_->listener != INVALID_SOCKET) {
        // Closed before joining, so the blocking accept() returns instead of holding the thread open
        // for the lifetime of the process.
        ::closesocket(impl_->listener);
        impl_->listener = INVALID_SOCKET;
    }
    // Release anyone waiting on an ABI answer BEFORE joining. A client blocked on a call that the
    // editor will now never pump would otherwise sit out the full timeout during shutdown, and the
    // join below would wait for that same thread.
    {
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
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->wsaUp) { WSACleanup(); impl_->wsaUp = false; }
    AVER_INFO("[Mcp] control channel stopped");
}

u32 McpBridge::pump(const std::function<void(const Command&)>& apply) {
    if (!impl_ || !apply) return 0;

    // ONE EVENT PER FRAME, not one command. This is the whole reason pacing lives on the main thread.
    //
    // A click is a move, a press and a release. ImGui registers a click only when one frame saw the
    // press and a LATER frame saw the release -- so delivering all three between two NewFrame calls
    // means nothing is ever clicked. The first version of this popped a whole Command per frame and
    // would have done exactly that: three events, one frame, no click, and a very confusing screenshot.
    //
    // So the front command is held and walked with a cursor, and only retired once its last event has
    // gone out. A click therefore takes three frames, which at 60 Hz is 50 ms and unnoticeable.
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
    // AN ABI CALL IS DISPATCHED HERE, not handed to `apply`. Two reasons, and the second is the point:
    // this module owns the registry, so it is the right place; and it is the only place on the MAIN
    // THREAD that knows the call has finished, which is what the waiting socket thread needs. Routing
    // it out to the app and back would put the answer somewhere nobody could return it from.
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
        // Still handed on, so the app can log it or act on it, but the reply no longer depends on that.
        apply(single);
        return 1;
    }

    apply(single);
    return 1;
}
#endif // _WIN32

} // namespace aver::mcp
