#pragma once
// Bindings and event routes, the runtime of the layout asset's BIND and EVENT records.
//
//   BIND Score text vm:Player.Score format "Score: {0:D6}"
//   BIND Health value var:Health mode change
//   BIND Warning visible vm:Player.Low
//   EVENT Play click command game.start
//   EVENT Quit click graph OnQuit
//   EVENT Volume valuechanged method Settings.SetVolume
//
// A binding copies a source value to one widget property, one way. The source is named by a prefix on the path:
// `vm:` a path on the host's view-model object (the C# side resolves it by reflection) or `var:` a graph variable the
// host publishes; a path with no prefix is a view-model path. The tree does not know either: the host installs a
// resolver (UiTree::setBindingResolver). `mode frame` (the default) reads the source every frame; `mode change` only
// when the host calls UiTree::bindingsChanged for that path. `format` shapes the value before it is set (see
// uiFormatBound).
//
// An event route sends a widget's event to a command (raised as a Command event, so every existing command listener
// sees it), a graph event or a method (both queued for the host to pick up with UiTree::pollRouted).
#include "aver/ui/UiTypes.hpp"
#include "aver/ui/UiWidget.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::ui {

enum class UiBindSource : u8 { ViewModel, Variable };

struct UiBindValue {
    enum class Kind : u8 { None, Number, Bool, Text };
    Kind kind = Kind::None;
    double number = 0;      // Number; Bool is 0 or 1
    std::string text;
};

struct UiBinding {
    UiWidgetId widget = 0;
    std::string property;       // a property key of the widget (UiProps.hpp)
    UiBindSource source = UiBindSource::ViewModel;
    std::string path;           // without the prefix
    std::string format;         // empty = the value as it reads
    bool onChange = false;      // mode change
    // runtime
    bool dirty = true;
    bool hasLast = false;
    std::string last;           // what was last set, so an unchanged value is not set again
};

// "vm:Player.Score", "var:Health", "Player.Score" -> source and path. False for an empty path.
bool uiParseBindPath(std::string_view text, UiBindSource& source, std::string& path);
std::string uiBindPathText(UiBindSource source, std::string_view path);   // with the prefix, vm: only when needed

// The value as text for a property. `format` holds {0}, {0:spec} or no placeholder (the whole string is a literal
// prefix then, and the value is appended). Specs: F<n> fixed decimals, D<n> integer padded to n digits, P<n> percent
// of a 0..1 value, N<n> grouped thousands, 0.00 / 0.# custom decimals, "a|b" picks a for true and b for false (bool),
// U / L upper and lower case (text). A number with no format prints as short as it reads back.
std::string uiFormatBound(const UiBindValue& v, std::string_view format);

enum class UiRouteAction : u8 { Command, Graph, Method };
const char* uiRouteActionName(UiRouteAction a);
bool uiRouteActionFromName(std::string_view s, UiRouteAction& out);

// Event names in the file: click, toggled, valuechanged, selectionchanged, textchanged, submitted, focus, blur,
// hover, unhover, cancel, command.
bool uiRouteEventFromName(std::string_view name, u8& type, bool& hoverEnter);   // type is a UiEventType
const char* uiRouteEventName(u8 type, bool hoverEnter);

struct UiEventRoute {
    UiWidgetId widget = 0;
    u8 type = 0;                // UiEventType
    bool hoverEnter = true;     // for Hovered: the route fires on enter (true) or leave (false)
    UiRouteAction action = UiRouteAction::Command;
    std::string target;
    std::vector<std::string> args;
    bool lastHover = false;     // runtime
};

// What a graph or method route queues for the host.
struct UiRoutedEvent {
    UiRouteAction action = UiRouteAction::Graph;
    std::string target;
    std::vector<std::string> args;
    UiWidgetId widget = 0;
    u8 eventType = 0;
    f32 value = 0;
    i32 index = 0;
    std::string text;           // the event's text
};

} // namespace aver::ui
