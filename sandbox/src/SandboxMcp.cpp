// Editor: the MCP control channel's commands and ABI registrations.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

namespace aver {
#if AVER_MODULE_MCP
// Applies one MCP command: an ABI call, or synthetic input posted to the window as Win32 messages.
void SandboxApp::applyMcpCommand(const mcp::Command& c) {
    HWND hwnd = window_ ? static_cast<HWND>(window_->nativeHandle()) : nullptr;
    if (!hwnd) return;

    if (c.name == "abi") {
        std::string result, why;
        if (mcp_.callAbi(c.abi, result, why))
            AVER_INFO("[Mcp] {}::{} -> {}", c.abi.module, c.abi.fn, result);
        else
            AVER_WARN("[Mcp] {}::{} refused: {}", c.abi.module, c.abi.fn, why);
        return;
    }

    for (const mcp::InputEvent& e : c.events) {
        const LPARAM lp = MAKELPARAM(e.x, e.y);
        switch (e.kind) {
            case mcp::InputEvent::Kind::MouseMove: {
                POINT pt{ e.x, e.y };
                ::ClientToScreen(hwnd, &pt);
                ::SetCursorPos(pt.x, pt.y);
                ::PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp);
                if (::GetForegroundWindow() != hwnd) ::SetForegroundWindow(hwnd);
                break;
            }
            case mcp::InputEvent::Kind::MouseDown:
                ::PostMessageW(hwnd,
                    e.button == 1 ? WM_RBUTTONDOWN : e.button == 2 ? WM_MBUTTONDOWN : WM_LBUTTONDOWN,
                    e.button == 1 ? MK_RBUTTON : e.button == 2 ? MK_MBUTTON : MK_LBUTTON, lp);
                break;
            case mcp::InputEvent::Kind::MouseUp:
                ::PostMessageW(hwnd,
                    e.button == 1 ? WM_RBUTTONUP : e.button == 2 ? WM_MBUTTONUP : WM_LBUTTONUP,
                    0, lp);
                break;
            case mcp::InputEvent::Kind::KeyDown:
                ::PostMessageW(hwnd, WM_KEYDOWN, static_cast<WPARAM>(e.key), 0);
                break;
            case mcp::InputEvent::Kind::KeyUp:
                ::PostMessageW(hwnd, WM_KEYUP, static_cast<WPARAM>(e.key), 0);
                break;
            case mcp::InputEvent::Kind::Text:
                // WM_CHAR per code unit: WM_KEYDOWN carries a virtual key, not a character.
                for (char ch : e.text)
                    ::PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(static_cast<unsigned char>(ch)), 0);
                break;
        }
    }
}

// Registers every built module's plain-C seam with the MCP bridge, under its own name.
void SandboxApp::registerMcpAbis() {
    mcp_.registerAbi("editor", [this](const mcp::AbiCall& a, std::string& r, std::string& w) {
        if (a.fn == "tool") {
            // kToolNames[4..7] are the sculpt tools, present only where AVER_MODULE_LANDSCAPE is
            // -- the bound below must track its size or an in-range request would read past it.
#if AVER_MODULE_LANDSCAPE
            constexpr int kMaxTool = 7;
#else
            constexpr int kMaxTool = 3;
#endif
            if (a.args.empty()) { w = "tool needs a tool index 0-" + std::to_string(kMaxTool); return false; }
            const int t = static_cast<int>(a.args[0]);
            if (t < 0 || t > kMaxTool) { w = "tool index out of range 0-" + std::to_string(kMaxTool); return false; }
            tool_ = static_cast<Tool>(t);
            r = kToolNames[t];
            return true;
        }
        if (a.fn == "screenshot") {
            if (a.text.empty()) { w = "screenshot needs a path in \"text\""; return false; }
            shot_ = a.text;
            capDone_ = false;
            r = a.text;
            return true;
        }
        if (a.fn == "mouse") {
#if AVER_WITH_IMGUI
            const ImGuiIO& io = ImGui::GetIO();
            char b[160];
            std::snprintf(b, sizeof b, "imgui pos=(%.0f,%.0f) display=(%.0f,%.0f) down=%d focus=%d",
                          io.MousePos.x, io.MousePos.y, io.DisplaySize.x, io.DisplaySize.y,
                          io.MouseDown[0] ? 1 : 0, io.AppFocusLost ? 0 : 1);
            r = b;
            return true;
#else
            w = "this build has no UI";
            return false;
#endif
        }
        if (a.fn == "widgets") {
            // THE REGISTRY IS THE EDITOR UI'S OWN. uiReg_ records which ImGui widget answers to which
        // name, so there is nothing for it to describe in a build with no editor UI -- and the
        // member itself is compiled out, which is what `no-ui` failed on. The command stays,
        // because an MCP client should get an answer rather than a closed socket.
#if AVER_WITH_IMGUI
        r = uiReg_.describe();
#else
        r = "{\"widgets\":[],\"note\":\"this build has no editor UI\"}";
#endif
            if (r.empty()) { w = "nothing tracked yet -- no UI frame has completed"; return false; }
            return true;
        }
        w = "editor has no entry point '" + a.fn + "'";
        return false;
    });
#if AVER_MODULE_PHYSICS
    mcp_.registerAbi("physics", [](const mcp::AbiCall& a, std::string& r, std::string& w) {
        if (a.fn == "bodyCount") { r = std::to_string(aver_phys_body_count()); return true; }
        if (a.fn == "ready")     { r = aver_phys_ready() ? "1" : "0"; return true; }
        w = "aver_phys_" + a.fn + " is not exposed";
        return false;
    });
#endif

    // ALWAYS REGISTERED, even when this build has no scene module: a client asking `modules` sees
    // "world" either way, and a call into it gets a clear "no scene module" refusal rather than
    // the generic "no ABI registered" one -- degrade the entry points, don't compile them out.
    mcp_.registerAbi("world", [this](const mcp::AbiCall& a, std::string& r, std::string& w) {
#if AVER_MODULE_SCENE
        if (a.fn == "stream_on") {
            setChunkStreamingEnabled(true);
            if (!streaming_.enabled()) {
                w = "streaming did not turn on -- see the editor log (no project open, or "
                    "ChunkWorld::open failed)";
                return false;
            }
            r = "on";
            return true;
        }
        if (a.fn == "stream_off") {
            setChunkStreamingEnabled(false);
            r = "off";
            return true;
        }
        if (a.fn == "stream_stats") {
            const world::StreamStats& s = streaming_.stats();
            char buf[320];
            std::snprintf(buf, sizeof buf,
                "streaming=%s residentChunks=%u residentEntities=%u loadedThisUpdate=%u "
                "evictedThisUpdate=%u pendingLoads=%u failedLoads=%u totalLoads=%u "
                "lastLoadMs=%.3f totalLoadMs=%.3f",
                streaming_.enabled() ? "on" : "off", s.residentChunks, s.residentEntities,
                s.loadedThisUpdate, s.evictedThisUpdate, s.pendingLoads, s.failedLoads,
                s.totalLoads, s.lastLoadMs, s.totalLoadMs);
            r = buf;
            return true;
        }
        if (a.fn == "warp") {
            if (a.args.size() < 3) { w = "warp needs 3 args: x y z (world centimetres)"; return false; }
            camPos_ = Vec3{static_cast<f32>(a.args[0]), static_cast<f32>(a.args[1]),
                           static_cast<f32>(a.args[2])};
            // A teleport, not a move: the next chunk-streaming update must not see this as a
            // huge one-frame velocity computed against wherever the camera used to be -- the
            // same rule frameCameraOnLevel's own comment states for the same reason.
            streaming_.resetVelocityTracking();
            char buf[96];
            std::snprintf(buf, sizeof buf, "camPos=(%.1f,%.1f,%.1f)", camPos_.x, camPos_.y, camPos_.z);
            r = buf;
            return true;
        }
        w = "world has no entry point '" + a.fn + "'";
        return false;
#else
        (void)a;
        w = "this build has no scene module (AVER_MODULE_SCENE=OFF) -- chunk streaming and the "
            "world ABI are unavailable";
        return false;
#endif
    });

    // ALWAYS REGISTERED too, for the same reason as "world" above.
    mcp_.registerAbi("graph", [this](const mcp::AbiCall& a, std::string& r, std::string& w) {
#if AVER_MODULE_SCENE
        if (a.fn == "entity_pos") {
            if (a.args.empty()) { w = "entity_pos needs an entity id"; return false; }
            const scene::Entity ent =
                static_cast<scene::Entity>(static_cast<u32>(static_cast<i32>(a.args[0])));
            if (!scene::World::instance().valid(ent)) { w = "no live entity with that id"; return false; }
            const Vec3 p = scene::World::instance().localTransform(ent).position;
            char buf[96];
            std::snprintf(buf, sizeof buf, "(%.3f,%.3f,%.3f)", p.x, p.y, p.z);
            r = buf;
            return true;
        }
#if AVER_MODULE_SCRIPTING
        // load/attach/tick: GraphHost is now hosted from native code via ScriptHost::graphLoad/
        // graphTick (same seam the drone entity uses). graphLoad binds a path AND an entity in one
        // call, so "load" just stages the path and "attach" is what actually calls it.
        if (a.fn == "load") {
            if (a.text.empty()) { w = "load needs a path in \"text\""; return false; }
            if (!scripts_.ready()) {
                w = "the scripting host is not running: " + scripts_.declineReason();
                return false;
            }
            if (!scripts_.graphAvailable()) {
                w = "this build's staged bridge exports no Graph entry points -- rebuild with the "
                    ".NET SDK present so Aver.Scripting.Bridge picks up GraphLoad/GraphTick/GraphUnload";
                return false;
            }
            mcpGraphPath_ = a.text;
            r = "path staged: " + mcpGraphPath_ + " -- call graph::attach <entityId> to load, "
                "compile and bind it";
            return true;
        }
        if (a.fn == "attach") {
            if (a.args.empty()) { w = "attach needs an entity id"; return false; }
            if (mcpGraphPath_.empty()) { w = "call graph::load <path> first"; return false; }
            const scene::Entity ent =
                static_cast<scene::Entity>(static_cast<u32>(static_cast<i32>(a.args[0])));
            if (!scene::World::instance().valid(ent)) { w = "no live entity with that id"; return false; }
            if (!scripts_.graphLoad(static_cast<i32>(ent), mcpGraphPath_)) {
                w = "graph failed to load/compile from '" + mcpGraphPath_ + "' -- see the [Graph] "
                    "error line just above in the editor log for why";
                return false;
            }
            mcpGraphEntity_ = ent;
            mcpGraphTimeSeconds_ = 0.0f;
            r = "attached entity #" + std::to_string(static_cast<u32>(ent)) + " to " + mcpGraphPath_;
            return true;
        }
        if (a.fn == "tick") {
            if (a.args.empty()) { w = "tick needs a seconds value (absolute time, not a delta)"; return false; }
            if (mcpGraphEntity_ == scene::kInvalidEntity) {
                w = "no entity attached -- call graph::attach <entityId> first";
                return false;
            }
            if (!scene::World::instance().valid(mcpGraphEntity_)) {
                w = "the attached entity no longer exists";
                return false;
            }
            mcpGraphTimeSeconds_ = static_cast<f32>(a.args[0]);
            scripts_.graphTick(static_cast<i32>(mcpGraphEntity_), mcpGraphTimeSeconds_);
            const Vec3 p = scene::World::instance().localTransform(mcpGraphEntity_).position;
            char buf[128];
            std::snprintf(buf, sizeof buf, "t=%.3f pos=(%.3f,%.3f,%.3f)",
                          mcpGraphTimeSeconds_, p.x, p.y, p.z);
            r = buf;
            return true;
        }
#else
        if (a.fn == "load" || a.fn == "attach" || a.fn == "tick") {
            w = "this build has no scripting module (AVER_MODULE_SCRIPTING=OFF) -- graph hosting is "
                "unavailable; entity_pos still works because it reads scene::World directly";
            return false;
        }
#endif
        w = "graph has no entry point '" + a.fn + "'";
        return false;
#else
        (void)a;
        w = "this build has no scene module (AVER_MODULE_SCENE=OFF) -- the graph ABI is unavailable";
        return false;
#endif
    });
}

// STARTS THE CHANNEL, WHOLE. See SandboxApp.hpp's own comment on this declaration for why the
// three steps below -- ABI registration, the widget hooks, and the listen -- moved here together
// rather than staying as onInit's private copy: a button that did only the last of those would
// bring up a channel that answers `ping` and nothing else.
bool SandboxApp::mcpStart(u16 port) {
    // REGISTERED ONCE. registerAbi replaces a module's dispatcher rather than appending to it, so
    // calling registerMcpAbis a second time would not itself be wrong -- but a stop/start cycle
    // (the status-bar widget's Stop then Start, or any future caller) running through every
    // module's registration again for no reason is exactly the kind of habit that stops being
    // harmless the day one of those registrations is not idempotent. mcpAbisRegistered_ is what
    // makes "once" true rather than "true so far".
    if (!mcpAbisRegistered_) {
        registerMcpAbis();
        mcpAbisRegistered_ = true;
    }
    // Widget hooks are UI-only: uiReg_ (the ImGui widget registry) doesn't exist with
    // AVER_ENABLE_UI=OFF, and there's nothing for MCP to resolve. Left unregistered -- MCP's
    // other ABIs still work, so a UI-less editor just reports no widgets. Installed on every call
    // rather than gated behind mcpAbisRegistered_ above: both closures are stateless captures of
    // `this`, so re-installing costs nothing, and doing it unconditionally is what keeps a
    // restarted channel's widget lookups working even if a future McpBridge::stop() ever clears
    // its own hooks.
#if AVER_WITH_IMGUI
    mcp_.setWidgetResolver([this](const std::string& n, f32& x, f32& y) {
        return uiReg_.centreOf(n, x, y);
    });
    mcp_.setWidgetLister([this] { return uiReg_.describe(); });
#endif
    if (!mcp_.start(port)) return false;
    // THE PORT THAT ACTUALLY STARTED, read back off the bridge rather than echoed from `port`:
    // today start() binds exactly what it is given or fails outright, so the two never differ, but
    // asking the bridge is what stays correct the day that stops being true.
    mcpPort_ = mcp_.port();
    return true;
}

// Stops the listener and nothing else. DOES NOT UNREGISTER ANYTHING -- there is no unregister call
// on McpBridge's surface to make even if this wanted to, and there would be nothing to gain by
// adding one: the ABI dispatchers and the widget hooks are stateless closures over `this`, so
// leaving them in place costs nothing while the socket is down, and mcpAbisRegistered_ above is
// what stops a later mcpStart from registering them a second time rather than this function having
// to undo them now.
void SandboxApp::mcpStop() {
    mcp_.stop();
}

#if AVER_WITH_IMGUI
// THE STATUS BAR'S CONTROL-CHANNEL WIDGET. Its default state is simply off -- nobody has to opt
// into a control channel, --mcp does, and most sessions never pass that flag -- which is the
// opposite of revision control's default state of "waiting for an answer". So idle is drawn as the
// ordinary case it is, and the one thing this widget insists on saying, in the tooltip and again in
// the menu, is what a live channel actually lets happen: it drives the editor with real input and
// can call any registered engine ABI, from anything on this machine that can reach the loopback
// port. A one-click Start that did not say that would be the wrong thing to ship.
void SandboxApp::drawMcpStatusWidget() {
    const bool live = mcp_.listening();
    char face[40];
    if (live) std::snprintf(face, sizeof face, ICON_LINK " MCP :%u", static_cast<unsigned>(mcp_.port()));
    else      std::snprintf(face, sizeof face, ICON_LINK " MCP");

    // NOT AN ALARM COLOUR WHEN OFF. Idle is the normal state for this widget, so painting it red or
    // amber would tell somebody something is wrong when nothing is -- it reads as live when live,
    // and otherwise no different from any other disabled label in the bar.
    const ImVec4 tint = live ? ImVec4(0.38f, 0.78f, 0.43f, 1.0f)
                              : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);

    const char* tooltip =
        live ? "MCP is listening on 127.0.0.1 only.\n"
               "Anything on this machine that can open a socket to that port can drive this "
               "editor -- move the mouse, type, click, and call a registered engine ABI.\n"
               "Click to stop it."
             : "MCP: a loopback control channel (127.0.0.1 only) that drives the editor with real "
               "input events and can call registered engine ABIs.\n"
               "Off by default. Click to start it.";

    // NAMED LIKE ITS NEIGHBOUR, and tracked by statusBarWidget itself rather than here: that
    // helper already publishes the control under the id it is handed, so a uiReg_.track() at this
    // call site would put one rect in the registry twice under two different names.
    if (statusBarWidget("statusbar.mcp", face, tint, tooltip))
        ImGui::OpenPopup("mcpStatusMenu");

    if (ImGui::BeginPopup("mcpStatusMenu")) {
        if (live) {
            if (ImGui::MenuItem("Stop")) mcpStop();
            uiReg_.track("statusbar.mcp.stop");
        } else {
            // mcpPort_ carries the last port asked for, INCLUDING a port a previous attempt failed
            // to bind -- so a retry from here offers that same value back rather than silently
            // falling to the default and binding somewhere the caller did not ask for.
            const u16 wantPort = mcpPort_ ? mcpPort_ : 45123;
            if (ImGui::MenuItem("Start")) {
                if (!mcpStart(wantPort))
                    AVER_WARN("[Mcp] Start (from the status bar) did not bring the channel up; "
                              "see the editor log above for why");
            }
            uiReg_.track("statusbar.mcp.start");
        }
        ImGui::Separator();
        // GREYED AND SPELLED OUT, not just implied by the icon: this is the same honesty as the
        // tooltip, kept visible without a hover for whoever already has the menu open.
        ImGui::TextDisabled("A loopback control channel (127.0.0.1 only).");
        ImGui::TextDisabled("Drives real input and calls registered engine ABIs.");
        if (live) ImGui::TextDisabled("Listening on 127.0.0.1:%u", static_cast<unsigned>(mcp_.port()));
        else      ImGui::TextDisabled("Not listening.");
        ImGui::EndPopup();
    }
}
#endif // AVER_WITH_IMGUI

#endif

} // namespace aver
