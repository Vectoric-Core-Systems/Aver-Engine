#pragma once
// Aver.Mcp — a control channel into a RUNNING editor, so a tool can press its buttons.
//
// WHAT IT IS FOR. Every visual defect in this tree was found by a human opening the editor and looking.
// The suites cannot do it: nine defects in one phase compiled, linked and passed everything
// (docs/STATUS.md 4u). This lets an agent drive the real editor -- move the pointer, click a tool,
// press a key -- and then look at what happened, which is the one kind of testing this repo has never
// been able to automate.
//
// HOW IT CLICKS, and this is the load-bearing decision. It POSTS REAL WIN32 MESSAGES to the window:
// WM_MOUSEMOVE, WM_LBUTTONDOWN/UP, WM_KEYDOWN/UP. The editor's input already arrives that way --
// ImGui_ImplWin32_WndProcHandler, see D3D12Device.cpp:217 -- so a synthetic click travels the identical
// path as a human one, through the same handler, in the same order, with no second code path to keep in
// step. The alternative was calling ImGui's io.Add*Event directly, which would fight the Win32
// backend's own NewFrame and would test a path no user ever takes.
//
// It also means THIS MODULE KNOWS NOTHING ABOUT IMGUI, or about the editor. It holds a window handle
// and a queue. That is why it can be optional.
//
// ================================================================================================
// THE EDITOR MUST WORK WITHOUT THIS MODULE, and that is a hard requirement rather than a nicety.
//
// Everything here is behind AVER_MODULE_MCP. With the switch off the target is not built, the header is
// not included, no thread starts, no socket is opened, no port is bound, and the editor has no idea it
// ever existed. A remote-control channel is exactly the kind of thing that must not be load-bearing:
// nobody should ship a game editor whose UI depends on a listening socket.
// ================================================================================================
//
// AND IT IS OFF BY DEFAULT EVEN WHEN BUILT. `start()` is called only when the app is asked to, because
// a build that silently listens on a port is a build that has opened a hole in somebody's machine
// without telling them. Loopback only, and it says on which port when it starts.
#include "aver/core/Types.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aver::mcp {

// One synthetic input, in the vocabulary the editor cares about rather than Win32's.
struct InputEvent {
    enum class Kind { MouseMove, MouseDown, MouseUp, KeyDown, KeyUp, Text } kind = Kind::MouseMove;
    i32 x = 0, y = 0;          // client pixels, for the mouse kinds
    i32 button = 0;            // 0 left, 1 right, 2 middle
    u32 key = 0;              // a virtual-key code, for the key kinds
    std::string text;          // for Text
};

// A call into ONE MODULE'S ABI.
//
// THE ROUTING RULE. A client says which module it wants and the name of an entry point, and this goes
// to that module's own plain-C seam -- Aver.Framework's request lands on framework_abi.h, Aver.Physics'
// on physics_abi.h, and so on. Presented to a client as one surface, "the Aver ABI", but there is no
// such single thing: it is the union of the seams where each module meets the engine core, and that is
// deliberately what it is.
//
// Why it matters that this is a lookup and not a switch: a switch here would mean Aver.Mcp knew the
// name of every module, and knowing them is one step from linking them. This target is Core-only, and
// staying that way is what lets the editor be built without it.
struct AbiCall {
    std::string module;        // "framework", "scene", "physics", "voxi", "ui", "audio", ...
    std::string fn;            // the entry point, without its module prefix: "spawn", not "aver_fw_spawn"
    std::vector<f64> args;     // numeric arguments, in order
    std::string text;          // one string argument, for the entries that take a name or a path
};

// What a client asked for, once it has been parsed off the wire.
struct Command {
    std::string name;                  // "click", "move", "key", "text", "ping", "shot", "abi"
    std::vector<InputEvent> events;    // already expanded: a click is a move, a down and an up
    std::string arg;                   // e.g. a screenshot path
    AbiCall abi;                       // filled when name == "abi"
    u64 id = 0;                        // echoed back, so a client can match reply to request
};

// How a module's ABI answers. Returns false and fills `why` when the entry point is unknown or the
// arguments are wrong -- refused with a reason, never silently ignored.
//
// `result` is free-form text the client gets back, so an entry that returns a handle or a count can say
// so without this module needing a type for it.
using AbiDispatch = std::function<bool(const AbiCall& call, std::string& result, std::string& why)>;

class McpBridge {
public:
    McpBridge();
    ~McpBridge();
    McpBridge(const McpBridge&) = delete;
    McpBridge& operator=(const McpBridge&) = delete;

    // Begin listening on 127.0.0.1:`port`. Returns false and logs why on failure -- a bridge that
    // could not bind must not look like one that did.
    //
    // LOOPBACK ONLY, hard-coded. This posts synthetic clicks into a running editor; it is not something
    // that should ever be reachable from another machine, and making the address configurable would be
    // offering that as an option.
    bool start(u16 port = 45123);
    void stop();
    bool listening() const;
    u16  port() const;

    // Drain whatever arrived and hand it to `apply`, which is expected to deliver the events to the
    // window. Call ONCE PER FRAME from the thread that owns the window.
    //
    // On the main thread on purpose, even though PostMessage is thread-safe: a click has to straddle a
    // frame to register -- press seen by one frame, release by the next -- so the pacing has to be
    // frame-aware, and the socket thread has no idea when a frame is.
    //
    // Returns how many commands were applied, so a caller can log activity rather than guess at it.
    u32 pump(const std::function<void(const Command&)>& apply);

    // ---- the ABI registry -------------------------------------------------------------------------
    //
    // INVERSION OF CONTROL, and it is the whole reason this module can stay Core-only. Aver.Mcp does not
    // call into Aver.Framework; the APP hands it a dispatcher for "framework" and Aver.Mcp forwards to
    // it, exactly the arrangement ActorEditorHooks uses to keep an asset editor from reaching into the
    // application.
    //
    // A pleasant consequence: the registry IS the build configuration. A module that was switched off
    // registers nothing, so `modules()` reports what this binary can actually reach and a call to a
    // missing one is refused with a reason instead of pretending.
    void registerAbi(const std::string& module, AbiDispatch dispatch);

    // Route a call. Refuses -- with a reason naming the module -- when nothing is registered under that
    // name, so "AVER_MODULE_PHYSICS was off in this build" is a diagnosable answer rather than silence.
    bool callAbi(const AbiCall& call, std::string& result, std::string& why) const;

    // Every module name with an ABI registered, sorted. What a client should ask for first.
    std::vector<std::string> modules() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Parse one line of the wire protocol into a Command. Exposed so it can be tested without a socket --
// the parsing is where the bugs live, and it should not need a listening port to check.
//
// The protocol is one JSON object per line, deliberately minimal:
//   {"id":1,"cmd":"move","x":100,"y":200}
//   {"id":2,"cmd":"click","x":100,"y":200,"button":"left"}
//   {"id":3,"cmd":"key","key":"F"}
//   {"id":4,"cmd":"text","text":"hello"}
//   {"id":5,"cmd":"ping"}
// Unknown commands are refused with a reason rather than ignored: a client that misspelled `click`
// should be told, not left waiting for a button that was never pressed.
bool parseCommand(const std::string& line, Command& out, std::string* why = nullptr);

} // namespace aver::mcp
