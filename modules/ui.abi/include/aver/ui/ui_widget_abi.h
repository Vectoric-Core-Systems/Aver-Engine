#pragma once
// Aver.UI widget system — the plain-C ABI over the retained widget tree (UiTree), the .ocui layout
// asset and the ready-made screens. The drawing half of Aver.UI.Abi (ui_abi.h) is unchanged and the
// two share one draw list: aver_ui_widgets_frame draws the widget tree into it.
//
// Handles are 32-bit widget ids; 0 is always "none". Strings are UTF-8. A returned `const char*`
// stays valid until the next call that returns a string.
#include "aver/ui/ui_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Widget kinds. Mirror aver::ui::UiWidgetKind.
#define AVER_UI_KIND_PANEL        0
#define AVER_UI_KIND_TEXT         1
#define AVER_UI_KIND_IMAGE        2
#define AVER_UI_KIND_BUTTON       3
#define AVER_UI_KIND_TOGGLE       4
#define AVER_UI_KIND_SLIDER       5
#define AVER_UI_KIND_CHOICE       6
#define AVER_UI_KIND_LIST         7
#define AVER_UI_KIND_SCROLL       8
#define AVER_UI_KIND_TEXT_INPUT   9
#define AVER_UI_KIND_PROGRESS     10
#define AVER_UI_KIND_KEYBIND      11

// Navigation actions the host feeds with aver_ui_input_nav. Mirror aver::ui::UiNav.
#define AVER_UI_NAV_UP        0
#define AVER_UI_NAV_DOWN      1
#define AVER_UI_NAV_LEFT      2
#define AVER_UI_NAV_RIGHT     3
#define AVER_UI_NAV_ACCEPT    4
#define AVER_UI_NAV_CANCEL    5
#define AVER_UI_NAV_NEXT_TAB  6
#define AVER_UI_NAV_PREV_TAB  7
#define AVER_UI_NAV_PAGE_UP   8
#define AVER_UI_NAV_PAGE_DOWN 9

// Text-editing keys for aver_ui_input_edit_key. Mirror aver::ui::UiEditKey.
#define AVER_UI_EDIT_BACKSPACE 0
#define AVER_UI_EDIT_DELETE    1
#define AVER_UI_EDIT_LEFT      2
#define AVER_UI_EDIT_RIGHT     3
#define AVER_UI_EDIT_HOME      4
#define AVER_UI_EDIT_END       5
#define AVER_UI_EDIT_ENTER     6

// Event types. Mirror aver::ui::UiEventType.
#define AVER_UI_EVENT_CLICKED           0   /* button / key-bind / list row activated */
#define AVER_UI_EVENT_TOGGLED           1   /* value = checked */
#define AVER_UI_EVENT_VALUE_CHANGED     2   /* slider value; choice (index = selected) */
#define AVER_UI_EVENT_SELECTION_CHANGED 3   /* list row (index) */
#define AVER_UI_EVENT_TEXT_CHANGED      4   /* text */
#define AVER_UI_EVENT_TEXT_SUBMITTED    5   /* text */
#define AVER_UI_EVENT_FOCUS_GAINED      6
#define AVER_UI_EVENT_FOCUS_LOST        7
#define AVER_UI_EVENT_KEY_CAPTURED      8   /* index = the raw key slot */
#define AVER_UI_EVENT_CANCEL            9
#define AVER_UI_EVENT_COMMAND           10  /* text = command: a button's `command`, "setting:<key>", "rebind:<action>:<slot>" */

// Bits of aver_ui_wants_input.
#define AVER_UI_WANTS_POINTER  1   /* the pointer is on UI, or a menu is open: gameplay must not see it */
#define AVER_UI_WANTS_KEYBOARD 2   /* a menu or a text field owns the keyboard / gamepad */
#define AVER_UI_WANTS_PAUSE    4   /* a menu is open: pause gameplay input */
#define AVER_UI_WANTS_CURSOR   8   /* show and free the OS cursor */

// ---- the frame (the host) ---------------------------------------------------------------------------

// Lays out and updates every visible root against the viewport given to aver_ui_begin_frame, routes
// this frame's input to it, raises events, and draws into the draw list. Call once per frame AFTER
// aver_ui_begin_frame and the game's HUD tick, BEFORE the draw list is submitted.
AVER_UI_API void aver_ui_widgets_frame(float dt);

// Input for the next aver_ui_widgets_frame. The pointer comes from aver_ui_set_pointer; the rest
// are edges ("happened since the last frame") and are cleared by the frame.
AVER_UI_API void aver_ui_input_nav(int32_t nav);
AVER_UI_API void aver_ui_input_char(uint32_t codepoint);
AVER_UI_API void aver_ui_input_edit_key(int32_t key);
AVER_UI_API void aver_ui_input_wheel(float notches);     /* positive = scroll up */
AVER_UI_API void aver_ui_input_raw_key(int32_t slot);    /* a key pressed this frame (framework slot), for rebinding */
AVER_UI_API void aver_ui_input_pointer_valid(int32_t valid);   /* 0 while the game owns the mouse (mouse-look) */

/* The one call a host needs for keyboard and gamepad: raw device state in, navigation, text editing,
 * typed characters (US layout) and key repeat out, accumulated for the next aver_ui_widgets_frame.
 * keyPressed/keyHeld are 256 bytes indexed by Win32 virtual key (mouse buttons VK 1, 2, 4 may be
 * included so a mouse button can be rebound); vkToSlot is 256 int16 mapping a virtual key to the
 * framework key slot a rebind should report (-1 = not rebindable), or NULL for no capture. The pad
 * arrays are AVER_FW_GAMEPAD_* buttons (14 bytes) and AVER_FW_GAMEPAD_AXIS_* axes (6 floats). Any
 * pointer may be NULL (treated as all zero). */
AVER_UI_API void aver_ui_input_host_frame(float dt, const uint8_t* keyPressed256, const uint8_t* keyHeld256,
                                          const int16_t* vkToSlot256, int32_t padConnected,
                                          const uint8_t* padButtons14, const float* padAxes6);

// What the host should do with its own input this frame (AVER_UI_WANTS_*), as of the last frame.
AVER_UI_API uint32_t aver_ui_wants_input(void);

// ---- environment ---------------------------------------------------------------------------------------

// "dark", "light" or "contrast".
AVER_UI_API void  aver_ui_set_theme(const char* name);
// mode: 0 constant, 1 width, 2 height, 3 shortest side, 4 blend. userScale is the player's setting.
AVER_UI_API void  aver_ui_set_dpi(int32_t mode, float refWidth, float refHeight, float userScale);
AVER_UI_API float aver_ui_scale(void);

// ---- structure ---------------------------------------------------------------------------------------------

// parent 0 makes a root. Returns the id, 0 on a bad parent or kind.
AVER_UI_API uint32_t aver_ui_widget_create(int32_t kind, uint32_t parent, const char* name);
AVER_UI_API int32_t  aver_ui_widget_destroy(uint32_t id);
AVER_UI_API int32_t  aver_ui_widget_reparent(uint32_t id, uint32_t newParent, int32_t index);
AVER_UI_API uint32_t aver_ui_widget_find(const char* name, uint32_t under);   /* under 0 = anywhere */
AVER_UI_API uint32_t aver_ui_widget_parent(uint32_t id);
AVER_UI_API uint32_t aver_ui_widget_root(uint32_t id);
AVER_UI_API int32_t  aver_ui_widget_child_count(uint32_t id);
AVER_UI_API uint32_t aver_ui_widget_child_at(uint32_t id, int32_t index);
AVER_UI_API int32_t  aver_ui_widget_kind(uint32_t id);                          /* -1 for an unknown id */
AVER_UI_API int32_t  aver_ui_widget_exists(uint32_t id);

// Any authored property by name ("anchors", "width", "text", ...) as text; see docs/GAME_UI.md for
// the list. set returns 1 on success, 0 for an unknown property or a bad value.
AVER_UI_API int32_t     aver_ui_widget_set_prop(uint32_t id, const char* key, const char* valueText);
AVER_UI_API const char* aver_ui_widget_get_prop(uint32_t id, const char* key);   /* NULL if unknown */

// Typed shortcuts for the values a game changes every frame.
AVER_UI_API void        aver_ui_widget_set_text(uint32_t id, const char* utf8);
AVER_UI_API const char* aver_ui_widget_get_text(uint32_t id);
AVER_UI_API void        aver_ui_widget_set_value(uint32_t id, float value);      /* slider, progress */
AVER_UI_API float       aver_ui_widget_get_value(uint32_t id);
AVER_UI_API void        aver_ui_widget_set_checked(uint32_t id, int32_t checked);
AVER_UI_API int32_t     aver_ui_widget_get_checked(uint32_t id);
AVER_UI_API void        aver_ui_widget_set_selected(uint32_t id, int32_t index); /* choice, list */
AVER_UI_API int32_t     aver_ui_widget_get_selected(uint32_t id);
AVER_UI_API void        aver_ui_widget_set_items(uint32_t id, const char* newlineSeparated);
AVER_UI_API void        aver_ui_widget_set_visible(uint32_t id, int32_t visible);
AVER_UI_API int32_t     aver_ui_widget_get_visible(uint32_t id);
AVER_UI_API void        aver_ui_widget_set_enabled(uint32_t id, int32_t enabled);
AVER_UI_API void        aver_ui_widget_set_texture(uint32_t id, uint64_t texture);   /* resolved image */

// Computed screen rectangle as of the last frame: x, y, width, height.
AVER_UI_API void aver_ui_widget_rect(uint32_t id, float* outXYWH);

AVER_UI_API int32_t  aver_ui_set_focus(uint32_t id);   /* 1 if it took focus; 0 clears focus */
AVER_UI_API uint32_t aver_ui_focused(void);
AVER_UI_API uint32_t aver_ui_hit_widget(float x, float y);   /* topmost widget under a point, 0 none */

// ---- layout assets (.ocui) -------------------------------------------------------------------------------------

// Parses .ocui text and creates its widgets as a new root. The HOST reads the file (this module does
// no file I/O). applyEnvironment 1 also takes the layout's theme and scale settings. Returns the
// root, or 0 with aver_ui_layout_error describing why.
AVER_UI_API uint32_t    aver_ui_layout_open(const char* utf8Text, int32_t applyEnvironment);
AVER_UI_API const char* aver_ui_layout_error(void);
// The subtree at `root` as .ocui text, for saving a layout built in code.
AVER_UI_API const char* aver_ui_layout_capture(uint32_t root);

// ---- events ------------------------------------------------------------------------------------------------------

// Pops the oldest event: returns 1 and fills the outputs, 0 when there is none. Text is written
// NUL-terminated into textBuf (truncated to textCap).
AVER_UI_API int32_t aver_ui_event_poll(int32_t* outType, uint32_t* outWidget, float* outValue,
                                       int32_t* outIndex, char* textBuf, int32_t textCap);

// ---- ready-made screens --------------------------------------------------------------------------------------------

// A centred titled menu of buttons. `entries` is one "Label|command" per line; a leading '!' on a
// label draws it dangerous. Returns its Menu-mode root. modal 1 dims the world behind it.
AVER_UI_API uint32_t aver_ui_menu_create(const char* rootName, const char* title, const char* entries,
                                         int32_t modal, const char* cancelCommand);

// The settings model. aver_ui_settings_reset_default fills graphics and audio items; add your own
// with aver_ui_settings_add (type 0 toggle, 1 slider, 2 choice; choices is newline separated).
AVER_UI_API void  aver_ui_settings_reset_default(void);
AVER_UI_API void  aver_ui_settings_clear(void);
AVER_UI_API void  aver_ui_settings_add(int32_t type, const char* key, const char* label, const char* tab,
                                       float value, float minValue, float maxValue, float step,
                                       const char* choices);
AVER_UI_API void  aver_ui_settings_set_value(const char* key, float value);
AVER_UI_API float aver_ui_settings_value(const char* key);

// The controls page's rows. Leave empty to build a settings screen without a Controls tab.
AVER_UI_API void aver_ui_rebind_clear(void);
AVER_UI_API void aver_ui_rebind_add(const char* action, int32_t slot, const char* label,
                                    const char* bindingText, int32_t rebindable);
AVER_UI_API void aver_ui_rebind_set_binding(int32_t row, const char* bindingText);

// Builds the settings screen from the models and returns its Menu-mode root (initially visible;
// hide it with aver_ui_widget_set_visible or the "ui.close" command). Replaces a previous one.
// Changes arrive as COMMAND events "setting:<key>" (value, index) and "rebind:<action>:<slot>"
// (index = raw key slot); the host applies them, then calls aver_ui_rebind_set_binding.
AVER_UI_API uint32_t aver_ui_settings_build(const char* rootName);

#ifdef __cplusplus
} // extern "C"
#endif
