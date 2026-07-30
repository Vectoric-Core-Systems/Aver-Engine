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

// What a client asked for, once it has been parsed off the wire.
struct Command {
    std::string name;                  // "click", "move", "key", "text", "ping", "shot"
    std::vector<InputEvent> events;    // already expanded: a click is a move, a down and an up
    std::string arg;                   // e.g. a screenshot path
    u64 id = 0;                        // echoed back, so a client can match reply to request
};

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
