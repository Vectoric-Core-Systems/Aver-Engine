// The editor control channel's wire protocol. Exit code = failure count. No socket, no window.
#include "aver/core/Log.hpp"
#include "aver/mcp/McpBridge.hpp"

#include <string>

using namespace aver;
using namespace aver::mcp;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

int main() {
    AVER_INFO("=== commands that should parse ===");
    {
        Command c;
        std::string why;

        check(parseCommand(R"({"id":7,"cmd":"ping"})", c, &why), "ping parses (" + why + ")");
        check(c.id == 7, "and its id is echoed back, so a client can match reply to request");
        check(c.events.empty(), "ping queues no input");

        check(parseCommand(R"({"cmd":"move","x":120,"y":340})", c, &why), "move parses");
        check(c.events.size() == 1 && c.events[0].kind == InputEvent::Kind::MouseMove,
              "as one MouseMove");
        check(c.events[0].x == 120 && c.events[0].y == 340, "with the coordinates intact");

        // A CLICK IS THREE EVENTS, expanded at parse time. ImGui registers a click only when one frame
        // saw the press and a later frame saw the release, so they have to be separable.
        check(parseCommand(R"({"cmd":"click","x":50,"y":60})", c, &why), "click parses");
        check(c.events.size() == 3, "into three events: move, down, up (got " +
                                   std::to_string(c.events.size()) + ")");
        check(c.events[0].kind == InputEvent::Kind::MouseMove &&
              c.events[1].kind == InputEvent::Kind::MouseDown &&
              c.events[2].kind == InputEvent::Kind::MouseUp, "in that order");
        check(c.events[1].button == 0, "left by default");

        check(parseCommand(R"({"cmd":"click","x":1,"y":2,"button":"right"})", c, &why), "a right click");
        check(c.events[1].button == 1, "carries button 1");
        check(parseCommand(R"({"cmd":"click","x":1,"y":2,"button":"middle"})", c, &why), "a middle click");
        check(c.events[1].button == 2, "carries button 2");

        // Keys BY NAME, because "F" and "F1" are different keys and a caller should not have to know
        // Win32's numbering to say either.
        check(parseCommand(R"({"cmd":"key","key":"F"})", c, &why), "a letter key parses");
        check(c.events.size() == 2 && c.events[0].key == 'F', "as down+up on the right code");
        check(parseCommand(R"({"cmd":"key","key":"f1"})", c, &why), "a named key parses");
        check(c.events[0].key == 0x70, "F1 is 0x70, not the letter F");
        check(parseCommand(R"({"cmd":"key","key":"ESCAPE"})", c, &why), "names are case-insensitive");
        check(c.events[0].key == 0x1B, "escape is 0x1B");

        check(parseCommand(R"({"cmd":"text","text":"hello"})", c, &why), "text parses");
        check(c.events.size() == 1 && c.events[0].text == "hello", "carrying the string");

        // A forward-slash path is what a caller should send: Windows accepts it, and this session
        // already learned the hard way that a literal backslash path in C++ can hide a control
        // character -- "\\Tools\\averdesign.exe" contains \\a, a BELL, and became
        // "binToolsverdesign.exe".
        check(parseCommand(R"({"cmd":"shot","path":"C:/tmp/a.png"})", c, &why), "shot parses");
        check(c.arg == "C:/tmp/a.png", "carrying the path (" + c.arg + ")");

        // And the escaped form, so the one level of unescaping is covered. THIS FIXTURE CAUGHT A
        // TEST BUG RATHER THAN A PARSER BUG: written through a shell heredoc it arrived with its
        // doubled backslashes collapsed, so the JSON held \\t and \\a, the parser consumed them
        // exactly as JSON says it should, and the expectation was simply wrong.
        check(parseCommand(R"({"cmd":"shot","path":"C:\\tmp\\a.png"})", c, &why), "an escaped path parses");
        check(c.arg == "C:\\tmp\\a.png", "with one level of escaping undone (" + c.arg + ")");

        // Whitespace and key order must not matter -- a client is entitled to pretty-print.
        check(parseCommand("{ \"cmd\" : \"move\" , \"y\" : 9 , \"x\" : 8 }", c, &why),
              "spacing and key order do not matter");
        check(c.events[0].x == 8 && c.events[0].y == 9, "and the values are still right");

        check(parseCommand(R"({"cmd":"move","x":-40,"y":-5})", c, &why), "negative coordinates parse");
        check(c.events[0].x == -40 && c.events[0].y == -5, "and stay negative");
    }

    AVER_INFO("=== commands that should be REFUSED, not ignored ===");
    {
        // Refused with a reason, in every case. A client that misspelled `click` should be told, not
        // left waiting for a button that was never pressed.
        Command c;
        std::string why;
        struct Case { const char* line; const char* what; };
        const Case cases[] = {
            {R"({"cmd":"clik","x":1,"y":2})",   "a misspelled command"},
            {R"({"x":1,"y":2})",                 "a command with no cmd field"},
            {R"({"cmd":"click","x":1})",         "a click missing y"},
            {R"({"cmd":"move"})",                "a move with no coordinates"},
            {R"({"cmd":"key"})",                 "a key with no key"},
            {R"({"cmd":"key","key":"nonsense"})","an unknown key name"},
            {R"({"cmd":"text"})",                "text with no text"},
            {R"({"cmd":"shot"})",                "shot with no path"},
            {"",                                  "an empty line"},
            {"not json at all",                   "something that is not JSON"},
        };
        for (const Case& k : cases) {
            why.clear();
            const bool refused = !parseCommand(k.line, c, &why);
            check(refused, std::string(k.what) + " is refused (" + why + ")");
        }
    }

    AVER_INFO("=== the bridge is inert until asked ===");
    {
        // Constructed but never started: no thread, no socket, no port. This is what "the editor works
        // without the control channel" looks like from the inside -- even when the module IS built, it
        // does nothing at all until the app calls start().
        McpBridge bridge;
        check(!bridge.listening(), "a fresh bridge is not listening");
        check(bridge.port() == 0, "and has no port");
        u32 applied = 99;
        applied = bridge.pump([](const Command&) {});
        check(applied == 0, "pumping an unstarted bridge is a no-op");
        // stop() on something never started must be safe, because the app's shutdown path does not know
        // whether start() succeeded.
        bridge.stop();
        check(!bridge.listening(), "stop() on an unstarted bridge is harmless");
    }

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
