#pragma once
// The editor's icon vocabulary: Material Icons codepoints, as UTF-8 string literals ready to
// concatenate into any ImGui label.
//
// EVERY CODEPOINT BELOW WAS CHECKED AGAINST THE FONT'S OWN cmap, not copied from documentation.
// Material Icons names its glyphs `uniXXXX`, so the file cannot tell you which icon is which and
// the name-to-codepoint mapping lives in a separate upstream file -- which means a wrong codepoint
// does not fail to build, it renders a different picture, or the atlas's notdef box. A newer
// release can and does move icons; see third_party/fonts/README.md for how to re-verify.
//
// WHY STRING LITERALS RATHER THAN wchar_t. ImGui takes UTF-8 const char*, and an icon is nearly
// always wanted INLINE with text -- ICON_FOLDER " Content" is one label, one draw, one hit-test
// rect. Encoding each as its own UTF-8 escape sequence lets that concatenation happen at compile
// time with no formatting call at all.
//
// The range these live in (U+E000-U+F8FF) is the Private Use Area, which is exactly why the icon
// font is MERGED into the body font's atlas rather than pushed as a separate font: no PUA codepoint
// can collide with real text, so one ImFont can serve both and a label needs no font push/pop.

namespace aver::editor {

// The Private Use Area span the icon font occupies. Handed to ImGui as the glyph range to build,
// so the atlas holds the icons rather than the whole 2188-glyph font.
inline constexpr unsigned short kIconRangeFirst = 0xE000;
inline constexpr unsigned short kIconRangeLast  = 0xF8FF;

} // namespace aver::editor

// ---- files and assets -------------------------------------------------------------------------
#define ICON_FOLDER      "\xEE\x8B\x87"   // U+E2C7 folder
#define ICON_FILE        "\xEE\x89\x8D"   // U+E24D insert_drive_file
#define ICON_SAVE        "\xEE\x85\xA1"   // U+E161 save
#define ICON_DELETE      "\xEE\xA1\xB2"   // U+E872 delete
#define ICON_EDIT        "\xEE\x8F\x89"   // U+E3C9 edit
#define ICON_LINK        "\xEE\x85\x97"   // U+E157 link

// ---- transport --------------------------------------------------------------------------------
#define ICON_PLAY        "\xEE\x80\xB7"   // U+E037 play_arrow
#define ICON_PAUSE       "\xEE\x80\xB4"   // U+E034 pause
#define ICON_STOP        "\xEE\x81\x87"   // U+E047 stop

// ---- editing ----------------------------------------------------------------------------------
#define ICON_ADD         "\xEE\x85\x85"   // U+E145 add
#define ICON_CLOSE       "\xEE\x97\x8D"   // U+E5CD close
#define ICON_UNDO        "\xEE\x85\xA6"   // U+E166 undo
#define ICON_REDO        "\xEE\x85\x9A"   // U+E15A redo
#define ICON_SEARCH      "\xEE\xA2\xB6"   // U+E8B6 search
#define ICON_REFRESH     "\xEE\x97\x95"   // U+E5D5 refresh

// ---- chrome and state -------------------------------------------------------------------------
#define ICON_SETTINGS    "\xEE\xA2\xB8"   // U+E8B8 settings
#define ICON_BUILD       "\xEE\xA1\xA9"   // U+E869 build
#define ICON_TUNE        "\xEE\x90\xA9"   // U+E429 tune
#define ICON_VISIBILITY  "\xEE\xA3\xB4"   // U+E8F4 visibility
#define ICON_INFO        "\xEE\xA2\x8E"   // U+E88E info
#define ICON_WARNING     "\xEE\x80\x82"   // U+E002 warning
#define ICON_ERROR       "\xEE\x80\x80"   // U+E000 error
#define ICON_CHEVRON     "\xEE\x97\x8C"   // U+E5CC chevron_right
#define ICON_EXPAND      "\xEE\x97\x8F"   // U+E5CF expand_more

// ---- domain -----------------------------------------------------------------------------------
#define ICON_AUDIO       "\xEE\x8E\xA1"   // U+E3A1 audiotrack
#define ICON_WAVE        "\xEE\x86\xB8"   // U+E1B8 graphic_eq
#define ICON_VOLUME      "\xEE\x81\x90"   // U+E050 volume_up
#define ICON_TERRAIN     "\xEE\x95\xA4"   // U+E564 terrain
#define ICON_TREE        "\xEE\xA5\xBA"   // U+E97A account_tree
