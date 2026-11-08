#pragma once
// Widget animation: tracks of keys over a widget's opacity, colours and render transform, sampled by time and
// played by a UiTree (UiTree::animPlay). The data is the layout asset's ANIM records:
//
//   ANIM Intro Panel opacity 0 0@out 0.3 1
//   ANIM Intro Panel renderTranslate 0 0,40@out 0.3 0,0
//   ANIM Intro Title bgColor 0 #FF000000 1 #FF0000FF
//
// `<time> <value>` pairs; a value is a float, `x,y` (a lone float fills both) or a colour `#RRGGBB[AA]`, with an
// optional `@ease` suffix that shapes the segment leaving that key (linear, step, in, out, inout, cubicin, cubicout,
// cubicinout; `in`/`out`/`inout` are the quadratic ones). The animation values sit over the authored ones and are
// never written back to the widget's properties.
#include "aver/ui/UiWidget.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::ui {

struct UiLayoutAnim;
struct UiLayoutDoc;

enum class UiEase : u8 { Linear, Step, QuadIn, QuadOut, QuadInOut, CubicIn, CubicOut, CubicInOut };
const char* uiEaseName(UiEase e);
bool uiEaseFromName(std::string_view s, UiEase& out);
f32 uiEaseApply(UiEase e, f32 x);   // x in 0..1 -> 0..1

// What a track drives. Float: Opacity, Rotation. Vec2: Translate, Scale. Colour: Bg, Text, Accent, Tint.
enum class UiAnimProp : u8 { Opacity, BgColor, TextColor, AccentColor, Tint, Translate, Rotation, Scale, Count };
enum class UiAnimKind : u8 { Float, Vec2, Colour };
UiAnimKind uiAnimPropKind(UiAnimProp p);
// The property key as the file and the property table write it ("opacity", "renderTranslate", ...). Aliases read:
// offset/translate, rotation/angle, scale, color/background, textcolor, accent, ...
const char* uiAnimPropName(UiAnimProp p);
bool uiAnimPropFromName(std::string_view s, UiAnimProp& out);

struct UiAnimKey {
    f32 time = 0;
    f32 v[4] = {0, 0, 0, 0};   // Float: v[0]; Vec2: v[0], v[1]; Colour: r, g, b, a in 0..255
    UiEase ease = UiEase::Linear;
};

struct UiAnimTrack {
    std::string widget;        // as the layout names it (nested: "Instance/Name")
    UiAnimProp prop = UiAnimProp::Opacity;
    std::vector<UiAnimKey> keys;   // ascending time
};

struct UiAnimation {
    std::string name;
    std::vector<UiAnimTrack> tracks;
    f32 length() const;        // the latest key time of any track
};

// The value of a track at `t` (held before the first key and after the last). False for an empty track.
struct UiAnimValue { f32 v[4] = {0, 0, 0, 0}; };
bool uiAnimSample(const UiAnimTrack& track, f32 t, UiAnimValue& out);

// ANIM record <-> track. The record's property text is kept as written when it parses, so the editor can show it.
bool uiAnimTrackFromRecord(const UiLayoutAnim& rec, UiAnimTrack& out, std::string* err = nullptr);
UiLayoutAnim uiAnimTrackToRecord(std::string_view animName, const UiAnimTrack& track);
// One key's value as a record token ("0.5@out", "0,40", "#FF0000FF") and back.
std::string uiAnimKeyToken(UiAnimProp p, const UiAnimKey& k);
bool uiAnimKeyFromToken(UiAnimProp p, std::string_view token, UiAnimKey& out);

// Groups a document's ANIM records into animations (records of one name, in file order); a record that does not
// parse is skipped and named in `errors`.
std::vector<UiAnimation> uiAnimationsFromDoc(const UiLayoutDoc& doc, std::vector<std::string>* errors = nullptr);

// Applies a sampled value to a widget's animated overrides.
void uiAnimApply(UiWidget& w, UiAnimProp p, const UiAnimValue& v);
// Drops the override of one property (the widget draws its authored value again).
void uiAnimRelease(UiWidget& w, UiAnimProp p);

} // namespace aver::ui
