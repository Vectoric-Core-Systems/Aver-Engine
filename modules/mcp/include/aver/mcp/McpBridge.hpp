#pragma once
// Aver.Mcp: a loopback control channel into a running editor, driving it with real Win32 messages.
// Built only under AVER_MODULE_MCP, and off until the app calls start().
#include "aver/core/Types.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace aver::mcp {

// One synthetic input, in the editor's vocabulary rather than Win32's.
struct InputEvent {
    enum class Kind { MouseMove, MouseDown, MouseUp, KeyDown, KeyUp, Text } kind = Kind::MouseMove;
    i32 x = 0, y = 0;          // client pixels, for the mouse kinds
    i32 button = 0;            // 0 left, 1 right, 2 middle
    u32 key = 0;              // a virtual-key code, for the key kinds
    std::string text;          // for Text
};

// A call into one module's ABI, routed by module name to that module's own plain-C seam.
struct AbiCall {
    std::string module;        // "framework", "scene", "physics", "voxi", "ui", "audio", ...
    std::string fn;            // the entry point, without its module prefix: "spawn", not "aver_fw_spawn"
    std::vector<f64> args;     // numeric arguments, in order
    std::string text;          // one string argument, for the entries that take a name or a path
};

// Where an ABI call's answer is left for the waiting socket thread to collect.
struct PendingResult {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string result;
    std::string why;
};

// What a client asked for, once it has been parsed off the wire.
struct Command {
    std::string name;                  // "click", "move", "key", "text", "ping", "shot", "abi"
    std::vector<InputEvent> events;    // already expanded: a click is a move, a down and an up
    std::string arg;                   // e.g. a screenshot path
    AbiCall abi;                       // filled when name == "abi"
    std::shared_ptr<PendingResult> pending;   // non-null when the caller waits on the outcome
    u64 id = 0;                        // echoed back, so a client can match reply to request
};

// How a module's ABI answers. Returns false and fills `why` when the entry or arguments are wrong.
using AbiDispatch = std::function<bool(const AbiCall& call, std::string& result, std::string& why)>;

// The control channel: a listening socket, a command queue, and the per-module ABI registry.
class McpBridge {
public:
    McpBridge();
    ~McpBridge();
    McpBridge(const McpBridge&) = delete;
    McpBridge& operator=(const McpBridge&) = delete;

    // Begins listening on 127.0.0.1:`port`, loopback only. Returns false and logs why on failure.
    bool start(u16 port = 45123);
    // Closes the socket, releases every waiting caller and joins the worker thread.
    void stop();
    bool listening() const;
    u16  port() const;

    // Delivers one queued input event to `apply` and returns how many commands were applied.
    // Call from the thread that owns the window.
    u32 pump(const std::function<void(const Command&)>& apply);

    // Registers (or replaces) the dispatcher a module's calls are routed to.
    void registerAbi(const std::string& module, AbiDispatch dispatch);

    // Routes a call to its module's dispatcher. Refuses, naming the module, when none is registered.
    bool callAbi(const AbiCall& call, std::string& result, std::string& why) const;

    // Every module name with an ABI registered, sorted.
    std::vector<std::string> modules() const;

    // Turns a widget name into a client-pixel point. Called on the socket thread; must be thread-safe.
    using WidgetResolver = std::function<bool(const std::string& name, f32& x, f32& y)>;
    // Installs the resolver used to expand a click-by-name into a coordinate click.
    void setWidgetResolver(WidgetResolver fn);

    // Returns the names the resolver would accept, for `{"cmd":"widgets"}`.
    using WidgetLister = std::function<std::string()>;
    // Installs the lister that answers a widgets request.
    void setWidgetLister(WidgetLister fn);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Parses one line of the wire protocol into a Command. Returns false and fills `why` if it will not
// parse. The wire format is one JSON object per line: {"id":2,"cmd":"click","x":1,"y":2,"button":"left"}
bool parseCommand(const std::string& line, Command& out, std::string* why = nullptr);

} // namespace aver::mcp
