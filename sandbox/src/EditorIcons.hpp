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
#define ICON_SKIP_NEXT   "\xEE\x81\x84"   // U+E044 skip_next (Play: Frame Skip)
#define ICON_EJECT       "\xEE\xA3\xBB"   // U+E8FB eject (Play: Eject)
#define ICON_GAMEPAD     "\xEE\x8C\xB8"   // U+E338 videogame_asset (Play: Possess)
#define ICON_MORE_VERT   "\xEE\x97\x94"   // U+E5D4 more_vert (Play: options)
#define ICON_OPEN_IN_NEW "\xEE\xA2\x9E"   // U+E89E open_in_new (Play: New Window)
#define ICON_SIMULATE    "\xEE\x81\xA8"   // U+E068 slow_motion_video (Play: Simulate)
#define ICON_PLATFORMS   "\xEE\x86\xB1"   // U+E1B1 devices (toolbar: Platforms)
#define ICON_WORLD_SPACE "\xEE\xA0\x8B"   // U+E80B public (viewport: world-space gizmo)
#define ICON_LOCAL_SPACE "\xEE\xA1\x8D"   // U+E84D 3d_rotation (viewport: local-space gizmo)

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

// ---- files and assets (additions) ------------------------------------------------------
#define ICON_FOLDER_OPEN       "\xEE\x8B\x88"   // U+E2C8 folder_open
#define ICON_NEW_FILE          "\xEE\xA2\x9C"   // U+E89C note_add
#define ICON_SAVE_ALL          "\xEE\x85\xB1"   // U+E171 save_alt
#define ICON_SCREENSHOT        "\xEE\x90\x92"   // U+E412 photo_camera
#define ICON_LAUNCH            "\xEE\xA2\x95"   // U+E895 launch
#define ICON_PACKAGE           "\xEE\x86\xA1"   // U+E1A1 inventory_2
#define ICON_EXIT              "\xEE\xA1\xB9"   // U+E879 exit_to_app
#define ICON_IMAGE             "\xEE\x8F\xB4"   // U+E3F4 image
#define ICON_MOVIE             "\xEE\x80\xAC"   // U+E02C movie

// ---- editing (additions) ---------------------------------------------------------------
#define ICON_COPY              "\xEE\x85\x8D"   // U+E14D content_copy
#define ICON_PASTE             "\xEE\x85\x8F"   // U+E14F content_paste
#define ICON_DUPLICATE         "\xEE\x8E\xBB"   // U+E3BB control_point_duplicate
#define ICON_SELECT_ALL        "\xEE\x85\xA2"   // U+E162 select_all
#define ICON_DESELECT          "\xEE\xAE\xB6"   // U+EBB6 deselect
#define ICON_RESET             "\xEE\xA2\x89"   // U+E889 history

// ---- windows, panels and drawers -------------------------------------------------------
#define ICON_OUTLINER          "\xEE\xA5\xBA"   // U+E97A account_tree
#define ICON_DETAILS           "\xEE\x83\xAE"   // U+E0EE list_alt
#define ICON_LOG               "\xEE\xBD\x82"   // U+EF42 article
#define ICON_CONSOLE           "\xEE\xAE\x8E"   // U+EB8E terminal
#define ICON_PROJECT           "\xEE\xA2\xB9"   // U+E8B9 settings_applications
#define ICON_WORLD             "\xEE\xA0\x8B"   // U+E80B public
#define ICON_LAYERS            "\xEE\x94\xBB"   // U+E53B layers
#define ICON_PROFILER          "\xEE\xA7\xA4"   // U+E9E4 speed
#define ICON_NEURAL            "\xEE\xA7\xB4"   // U+E9F4 hub
#define ICON_EXTENSION         "\xEE\xA1\xBB"   // U+E87B extension
#define ICON_REVISION          "\xEE\x82\xB6"   // U+E0B6 call_split
#define ICON_WIDGETS           "\xEE\x86\xBD"   // U+E1BD widgets
#define ICON_LAYOUT            "\xEE\xA1\xB1"   // U+E871 dashboard
#define ICON_HELP              "\xEE\xA3\xBD"   // U+E8FD help_outline
#define ICON_ACCESSIBILITY     "\xEE\xA4\xAC"   // U+E92C accessibility_new
#define ICON_MCP               "\xEE\xBF\xA6"   // U+EFE6 cable
#define ICON_MAP               "\xEE\x95\x9B"   // U+E55B map
#define ICON_BOT               "\xEF\x81\xAC"   // U+F06C smart_toy
#define ICON_LIGHTBULB_OUTLINE "\xEE\xA4\x8F"   // U+E90F lightbulb_outline

// ---- actor types (World Outliner rows and the Add menu) --------------------------------
#define ICON_ACTOR_MESH        "\xEE\xA7\xBE"   // U+E9FE view_in_ar
#define ICON_ACTOR             "\xEE\x95\xB4"   // U+E574 category
#define ICON_LIGHT             "\xEE\x83\xB0"   // U+E0F0 lightbulb
#define ICON_SPOT_LIGHT        "\xEE\x90\xAE"   // U+E42E wb_incandescent
#define ICON_RECT_LIGHT        "\xEE\x8F\x86"   // U+E3C6 crop_square
#define ICON_CAMERA            "\xEE\x81\x8B"   // U+E04B videocam
#define ICON_SUN               "\xEE\x90\xB0"   // U+E430 wb_sunny
#define ICON_SKY               "\xEE\x8A\xBD"   // U+E2BD cloud
#define ICON_POST_PROCESS      "\xEE\x8E\xA5"   // U+E3A5 blur_on
#define ICON_PLAYER_START      "\xEE\x95\xAA"   // U+E56A person_pin_circle
#define ICON_CLASS             "\xEE\xA1\xAF"   // U+E86F code
#define ICON_PARTICLES         "\xEE\x8F\xAA"   // U+E3EA grain
#define ICON_DECAL             "\xEE\x90\xA1"   // U+E421 texture
#define ICON_SKELETAL          "\xEE\x95\xA6"   // U+E566 directions_run
#define ICON_SPHERE            "\xEE\xA0\xB6"   // U+E836 radio_button_unchecked
#define ICON_SHAPES            "\xEE\x95\xB4"   // U+E574 category

// ---- details categories ----------------------------------------------------------------
#define ICON_TRANSFORM         "\xEE\xA2\x9F"   // U+E89F open_with
#define ICON_MATERIAL          "\xEE\x90\x8A"   // U+E40A palette
#define ICON_BRUSH             "\xEE\x8E\xAE"   // U+E3AE brush
#define ICON_PHYSICS           "\xEE\xA8\x8B"   // U+EA0B bolt
#define ICON_ANIMATION         "\xEE\x9C\x9C"   // U+E71C animation
#define ICON_HAIR              "\xEE\xBF\x98"   // U+EFD8 air
#define ICON_WATER             "\xEE\x9E\x98"   // U+E798 water_drop

// ---- outliner ---------------------------------------------------------------------------
#define ICON_VISIBILITY_OFF    "\xEE\xA3\xB5"   // U+E8F5 visibility_off
#define ICON_LOCK              "\xEE\xA2\x97"   // U+E897 lock
#define ICON_LOCK_OPEN         "\xEE\xA2\x98"   // U+E898 lock_open
#define ICON_FILTER            "\xEE\x85\x92"   // U+E152 filter_list
#define ICON_NEW_FOLDER        "\xEE\x8B\x8C"   // U+E2CC create_new_folder
#define ICON_ARROW_BACK        "\xEE\x97\x84"   // U+E5C4 arrow_back (Content Browser: Back)
#define ICON_ARROW_FWD         "\xEE\x97\x88"   // U+E5C8 arrow_forward (Content Browser: Forward)
#define ICON_ARROW_UP          "\xEE\x97\x98"   // U+E5D8 arrow_upward (Content Browser: Up)
#define ICON_IMPORT            "\xEE\x8B\x84"   // U+E2C4 file_download (Content Browser: Import)
#define ICON_VIEW_TILES        "\xEE\xA3\xB0"   // U+E8F0 view_module (Content Browser: tiles)
#define ICON_VIEW_LIST         "\xEE\xA3\xAF"   // U+E8EF view_list (Content Browser: list)

// ---- content browser --------------------------------------------------------------------
#define ICON_STAR              "\xEE\xA0\xB8"   // U+E838 star
#define ICON_STAR_BORDER       "\xEE\xA0\xBA"   // U+E83A star_border
#define ICON_COLLECTION        "\xEE\x90\xB1"   // U+E431 collections_bookmark

// ---- toasts, status bar, panels, viewport options (editor polish 8-10) ----------------------
#define ICON_CHECK_CIRCLE       "\xEE\xA1\xAC"   // U+E86C check_circle
#define ICON_GRID_ON            "\xEE\x8F\xAC"   // U+E3EC grid_on
#define ICON_ROTATE             "\xEE\x90\x9A"   // U+E41A rotate_right
#define ICON_SCALE              "\xEE\xA1\x9B"   // U+E85B aspect_ratio
#define ICON_VIEW_SPLIT         "\xEE\xA5\x89"   // U+E949 vertical_split
#define ICON_VIEW_QUAD          "\xEE\xA3\xB0"   // U+E8F0 view_module
#define ICON_CLEAR_ALL          "\xEE\x85\xAC"   // U+E16C delete_sweep

// ---- graph editor toolbar ---------------------------------------------------------------------
#define ICON_BUG                "\xEE\xA1\xA8"   // U+E868 bug_report
#define ICON_FRAME_ALL          "\xEE\xA8\x90"   // U+EA10 fit_screen
#define ICON_AUTO_LAYOUT        "\xEE\x99\xA3"   // U+E663 auto_fix_high
