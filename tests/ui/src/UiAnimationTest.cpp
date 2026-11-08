// Headless test for widget animation: easing, sampling, the ANIM record round trip, and the player on a UiTree (play,
// reverse, loop, ping-pong, stop, seek, the finish event), plus the render transform reaching the draw list.
#include "aver/core/Log.hpp"
#include "aver/ui/UiAnimation.hpp"
#include "aver/ui/UiLayoutAsset.hpp"
#include "aver/ui/UiText.hpp"
#include "aver/ui/UiTree.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::ui;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f32 a, f32 b, f32 eps = 0.01f) { return std::fabs(a - b) <= eps; }

static UiEstimatedMetrics g_metrics;

static const char* kDoc =
    "OCUI 2\n"
    "WIDGET Root panel -\n"
    "WIDGET Box panel Root\n"
    "SET Box width 100\nSET Box height 50\n"
    "WIDGET Label text Root\n"
    "ANIM Intro Box opacity 0 0 1 1\n"
    "ANIM Intro Box renderTranslate 0 0,0@out 1 100,50\n"
    "ANIM Intro Label bgColor 0 #00000000 2 #FF0000FF\n"
    "ANIM Pulse Box renderScale 0 1 0.5 2@inout 1 1\n";

static UiLayoutDoc parseDoc() {
    UiLayoutDoc doc;
    std::string err;
    check(uiParseLayout(kDoc, doc, &err), "the test layout parses: " + err);
    return doc;
}

static usize drainFinished(UiTree& t, std::vector<UiEvent>* keep = nullptr) {
    usize n = 0;
    UiEvent e;
    while (t.pollEvent(e))
        if (e.type == UiEventType::AnimationFinished) { ++n; if (keep) keep->push_back(e); }
    return n;
}

int main() {
    AVER_INFO("=== easing ===");
    {
        UiEase e;
        check(uiEaseFromName("InOut", e) && e == UiEase::QuadInOut && !uiEaseFromName("bounce", e), "ease names are case-insensitive and checked");
        check(approx(uiEaseApply(UiEase::Linear, 0.25f), 0.25f) && approx(uiEaseApply(UiEase::QuadIn, 0.5f), 0.25f) &&
                  approx(uiEaseApply(UiEase::QuadOut, 0.5f), 0.75f) && approx(uiEaseApply(UiEase::CubicInOut, 0.5f), 0.5f),
              "linear, in, out and in-out shape the segment");
        check(uiEaseApply(UiEase::Step, 0.99f) == 0.0f && uiEaseApply(UiEase::Step, 1.0f) == 1.0f && uiEaseApply(UiEase::QuadIn, 7.0f) == 1.0f,
              "step holds until the end, and the input is clamped");
    }

    AVER_INFO("=== records and sampling ===");
    {
        UiLayoutAnim rec{"Intro", "Box", "offset", {"0", "0,0@out", "1", "10,20"}};
        UiAnimTrack t;
        std::string err;
        check(uiAnimTrackFromRecord(rec, t, &err) && t.prop == UiAnimProp::Translate && t.keys.size() == 2 && t.keys[0].ease == UiEase::QuadOut,
              "an offset track parses with its alias and ease");
        const UiLayoutAnim back = uiAnimTrackToRecord("Intro", t);
        check(back.property == "renderTranslate" && back.keys[1] == "0,0@out", "and writes back under the canonical name");
        check(!uiAnimTrackFromRecord({"A", "B", "opacity", {"0"}}, t) && !uiAnimTrackFromRecord({"A", "B", "nonsense", {}}, t) &&
                  !uiAnimTrackFromRecord({"A", "B", "opacity", {"1", "1", "0", "0"}}, t) && !uiAnimTrackFromRecord({"A", "B", "tint", {"0", "red"}}, t),
              "odd keys, an unknown property, backwards times and a bad colour are refused");

        UiAnimTrack op;
        uiAnimTrackFromRecord({"A", "W", "opacity", {"1", "0.2", "3", "1"}}, op);
        UiAnimValue v;
        check(uiAnimSample(op, 0.0f, v) && approx(v.v[0], 0.2f) && uiAnimSample(op, 9.0f, v) && approx(v.v[0], 1.0f), "held before the first key and after the last");
        check(uiAnimSample(op, 2.0f, v) && approx(v.v[0], 0.6f), "linear between keys");
        UiAnimTrack col;
        uiAnimTrackFromRecord({"A", "W", "bgColor", {"0", "#00000000", "1", "#FF804020"}}, col);
        check(uiAnimSample(col, 0.5f, v) && approx(v.v[0], 127.5f, 1.0f) && approx(v.v[3], 16.0f, 1.0f), "colours blend per channel");
        UiAnimTrack none;
        check(!uiAnimSample(none, 0.0f, v), "an empty track has no value");
    }

    AVER_INFO("=== the document ===");
    {
        UiLayoutDoc doc = parseDoc();
        const std::vector<UiAnimation> anims = uiAnimationsFromDoc(doc);
        check(anims.size() == 2 && anims[0].name == "Intro" && anims[0].tracks.size() == 3 && approx(anims[0].length(), 2.0f), "records group by animation name");
        const std::string text = uiWriteLayout(doc);
        UiLayoutDoc again;
        check(uiParseLayout(text, again) && uiWriteLayout(again) == text, "the layout still round-trips byte for byte");
        UiLayoutDoc bad = doc;
        bad.anims.push_back({"Broken", "Box", "opacity", {"x"}});
        std::vector<std::string> errs;
        check(uiAnimationsFromDoc(bad, &errs).size() == 2 && errs.size() == 1, "a record that does not parse is skipped and reported");
    }

    AVER_INFO("=== the player ===");
    {
        UiTree t;
        UiLayoutDoc doc = parseDoc();
        const UiInstantiateResult built = uiInstantiateLayoutEx(doc, t, 0, {});
        const UiWidgetId root = built.root;
        const UiWidgetId box = t.find("Box");
        const UiWidgetId label = t.find("Label");
        check(root != 0 && t.findAnimation(root, "Intro") && t.findAnimation(root, "Pulse") && !t.findAnimation(root, "Nope"), "instantiate attaches the animations to the root");
        check(!t.animPlay(root, "Nope") && !t.animPlay(9999, "Intro"), "an unknown animation or root does not play");

        check(t.animPlay(root, "Intro") && t.animPlaying(root, "Intro"), "play starts it");
        check(approx(t.get(box)->fx.opacity, 0.0f) && (t.get(box)->fx.mask & kFxOpacity), "the first frame is applied at once");
        t.animAdvance(0.5f);
        check(approx(t.get(box)->fx.opacity, 0.5f) && approx(t.animTime(root, "Intro"), 0.5f), "half a second in, opacity is half");
        check(approx(t.get(box)->fx.translate.x, 75.0f) && approx(t.get(box)->fx.translate.y, 37.5f), "the out ease leads the offset (75% at half time)");
        check(approx(t.get(box)->opacity, 1.0f), "the authored opacity is untouched");
        t.animAdvance(1.0f);
        check(approx(t.get(box)->fx.opacity, 1.0f) && approx(t.animTime(root, "Intro"), 1.5f), "past a track's end its value holds while the longer track runs");
        t.animAdvance(1.0f);
        std::vector<UiEvent> fin;
        check(!t.animPlaying(root, "Intro") && t.animTime(root, "Intro") < 0.0f, "it ends once its longest track does");
        check(drainFinished(t, &fin) == 0, "the finish event arrives with the next dispatch, not before");
        t.update(0.0f, UiInputFrame{}, g_metrics);
        check(drainFinished(t, &fin) == 1 && fin[0].widget == root && fin[0].text == "Intro" && approx(fin[0].value, 2.0f), "exactly one finish event, named for the animation");
        check(approx(t.get(label)->fx.bg == 0 ? 0.0f : static_cast<f32>(t.get(label)->fx.bg >> 24), 255.0f, 1.0f), "the last values stay after it finished");

        check(t.animStop(root, "Intro") && !(t.get(box)->fx.mask & kFxOpacity) && t.get(label)->fx.mask == 0, "stop releases what it drove");

        UiAnimPlay rev;
        rev.reverse = true;
        t.animPlay(root, "Intro", rev);
        check(approx(t.animTime(root, "Intro"), 2.0f), "reverse starts at the end");
        t.animAdvance(1.0f);
        check(approx(t.animTime(root, "Intro"), 1.0f) && approx(t.get(box)->fx.opacity, 1.0f), "and runs backwards");
        t.animAdvance(1.5f);
        t.update(0.0f, UiInputFrame{}, g_metrics);
        check(drainFinished(t) == 1 && approx(t.get(box)->fx.opacity, 0.0f), "reaching the start finishes it at its first values");

        t.animPlay(root, "Intro");
        t.animAdvance(0.5f);
        check(t.animReverse(root, "Intro") && approx(t.animTime(root, "Intro"), 0.5f), "reverse flips a running animation in place");
        t.animAdvance(0.25f);
        check(approx(t.animTime(root, "Intro"), 0.25f), "it then runs back");
        t.animStop(root, "Intro");

        UiAnimPlay loop;
        loop.loops = 0;
        t.animPlay(root, "Pulse", loop);
        t.animAdvance(2.5f);
        t.update(0.0f, UiInputFrame{}, g_metrics);
        check(t.animPlaying(root, "Pulse") && approx(t.animTime(root, "Pulse"), 0.5f) && drainFinished(t) == 0, "loops 0 wraps and never finishes");
        t.animStop(root, "Pulse");

        UiAnimPlay twice;
        twice.loops = 2;
        t.animPlay(root, "Pulse", twice);
        t.animAdvance(1.5f);
        check(t.animPlaying(root, "Pulse") && approx(t.animTime(root, "Pulse"), 0.5f), "two plays: wrapped once");
        t.animAdvance(1.0f);
        t.update(0.0f, UiInputFrame{}, g_metrics);
        std::vector<UiEvent> f2;
        check(!t.animPlaying(root, "Pulse") && drainFinished(t, &f2) == 1 && f2[0].index == 2, "and finished after the second, reporting two loops");

        UiAnimPlay pp;
        pp.loops = 0;
        pp.pingPong = true;
        t.animPlay(root, "Pulse", pp);
        t.animAdvance(1.25f);
        check(approx(t.animTime(root, "Pulse"), 0.75f), "ping-pong turns round at the end");
        t.animAdvance(0.5f);
        check(approx(t.animTime(root, "Pulse"), 0.25f), "and keeps going back");
        t.animStop(root, "Pulse");

        UiAnimPlay fast;
        fast.speed = 2.0f;
        t.animPlay(root, "Pulse", fast);
        t.animAdvance(0.25f);
        check(approx(t.animTime(root, "Pulse"), 0.5f), "speed scales time");
        check(t.animPause(root, "Pulse", true) && !t.animPlaying(root, "Pulse"), "pause holds");
        t.animAdvance(5.0f);
        check(approx(t.animTime(root, "Pulse"), 0.5f), "a paused animation does not move");
        t.animStop(root, "Pulse");

        check(t.animSeek(root, "Intro", 1.0f) && approx(t.get(box)->fx.opacity, 1.0f) && !t.animPlaying(root, "Intro"), "seek applies a time without playing");
        check(t.animSeek(root, "Intro", 0.5f) && approx(t.get(box)->fx.opacity, 0.5f), "and again, for scrubbing");
        check(t.animSeek(root, "Intro", 99.0f) && approx(t.animTime(root, "Intro"), 2.0f), "a seek is clamped to the length");
        t.animPlay(root, "Intro");
        t.destroy(root);
        t.update(0.5f, UiInputFrame{}, g_metrics);
        check(t.runtimeOf(root) == nullptr && !t.animPlaying(root, "Intro"), "destroying the root drops its animations and runs");
    }

    AVER_INFO("=== nested layouts ===");
    {
        UiTree t;
        UiLayoutDoc inner;
        uiParseLayout("OCUI 2\nWIDGET Row panel -\nWIDGET Dot panel Row\nANIM Blink Dot opacity 0 1 1 0\n", inner);
        UiLayoutDoc outer;
        uiParseLayout("OCUI 2\nWIDGET Root panel -\nUSE Item \"Row.ocui\" Root\n", outer);
        const UiLayoutLoader loader = [&](const std::string&, std::string& text) { text = uiWriteLayout(inner); return true; };
        const UiInstantiateResult built = uiInstantiateLayoutEx(outer, t, 0, loader);
        check(built.root != 0 && t.findAnimation(built.root, "Item/Blink") != nullptr, "a used layout's animations come over as <instance>/<name>");
        check(t.animPlay(built.root, "Item/Blink") && t.animSeek(built.root, "Item/Blink", 0.5f) && approx(t.get(t.find("Item/Dot"))->fx.opacity, 0.5f),
              "and drive the widgets built under the instance's name");
    }

    AVER_INFO("=== the render transform ===");
    {
        UiTree t;
        const UiWidgetId rid = t.create(UiWidgetKind::Panel, 0, "Root");
        t.get(rid)->style = "clear";
        const UiWidgetId id = t.create(UiWidgetKind::Image, rid, "Img");
        UiWidget* w = t.get(id);
        w->width = 100;
        w->height = 50;
        w->anchors = UiAnchors{0, 0, 0, 0};
        t.setViewport(UiRect{0, 0, 800, 600});
        t.layout(g_metrics);
        const auto firstQuad = [&](UiVertex* corner) {
            UiDrawList list;
            UiDrawListPainter p(list, nullptr);
            t.draw(p);
            for (usize i = 0; i < list.vertices().size(); ++i) corner[i < 4 ? i : 3] = list.vertices()[i < 4 ? i : 3];
            return list.vertices().size();
        };
        UiVertex plain[4];
        const usize n0 = firstQuad(plain);
        check(n0 >= 4, "the widget draws a quad");
        w->renderTranslate = {10, 20};
        UiVertex moved[4];
        firstQuad(moved);
        check(approx(moved[0].x - plain[0].x, 10.0f * t.scale()) && approx(moved[0].y - plain[0].y, 20.0f * t.scale()), "an authored render translate shifts the draw");
        const UiRect laidOut = t.get(id)->rect;
        t.layout(g_metrics);
        check(laidOut == t.get(id)->rect, "layout ignores the transform");
        w->renderTranslate = {0, 0};
        w->renderScale = {2, 2};
        UiVertex scaled[4];
        firstQuad(scaled);
        const f32 cx = laidOut.x + laidOut.w * 0.5f;
        check(approx(scaled[0].x, cx - (cx - plain[0].x) * 2.0f, 0.1f), "scale grows around the pivot (the centre by default)");
        w->renderScale = {1, 1};
        w->renderRotation = 90.0f;
        UiVertex turned[4];
        firstQuad(turned);
        const f32 cy = laidOut.y + laidOut.h * 0.5f;
        check(approx(turned[0].x, cx - (plain[0].y - cy), 0.1f) && approx(turned[0].y, cy + (plain[0].x - cx), 0.1f), "a quarter turn rotates the corners about the pivot");
        w->renderRotation = 0.0f;
        w->fx.translate = {5, 0};
        w->fx.mask |= kFxTranslate;
        UiVertex anim[4];
        firstQuad(anim);
        check(approx(anim[0].x - plain[0].x, 5.0f * t.scale()), "an animated value replaces the authored one");

        UiTree t2;
        const UiWidgetId r2 = t2.create(UiWidgetKind::Panel, 0, "Root");
        t2.get(r2)->bgColor = uiRgba(10, 20, 30, 255);
        t2.get(r2)->fx.bg = uiRgba(200, 0, 0, 255);
        t2.get(r2)->fx.mask |= kFxBg;
        t2.setViewport(UiRect{0, 0, 100, 100});
        t2.layout(g_metrics);
        UiDrawList list2;
        UiDrawListPainter p2(list2, nullptr);
        t2.draw(p2);
        check(!list2.vertices().empty() && (list2.vertices()[0].rgba & 0xFF) == 200, "an animated colour replaces the authored one in the draw");
    }

    AVER_INFO("=== per-state colours ===");
    {
        UiTree t;
        const UiWidgetId id = t.create(UiWidgetKind::Panel, 0, "Root");
        UiWidget* w = t.get(id);
        w->bgColor = uiRgba(10, 20, 30, 255);
        w->bgHoverColor = uiRgba(200, 0, 0, 255);
        w->bgPressedColor = uiRgba(0, 200, 0, 255);
        w->bgDisabledColor = uiRgba(0, 0, 200, 255);
        t.setViewport(UiRect{0, 0, 100, 100});
        t.layout(g_metrics);
        const auto firstRed = [&]() {
            UiDrawList list;
            UiDrawListPainter p(list, nullptr);
            t.draw(p);
            return list.vertices().empty() ? 0u : list.vertices()[0].rgba & 0xFF;
        };
        check(firstRed() == 10, "normal uses bgColor");
        w->hovered = true;
        check(firstRed() == 200, "hovered uses bgHoverColor");
        w->pressed = true;
        check(firstRed() == 0, "pressed uses bgPressedColor");
        w->hovered = w->pressed = false;
        w->enabled = false;
        UiDrawList list;
        UiDrawListPainter p(list, nullptr);
        t.draw(p);
        check(!list.vertices().empty() && ((list.vertices()[0].rgba >> 16) & 0xFF) > 150, "disabled uses bgDisabledColor");
        w->enabled = true;
        w->bgHoverColor = 0;
        w->hovered = true;
        check(firstRed() != 200 && firstRed() > 10, "with no state colour the old derived shade applies");
    }

    if (g_failures) {
        AVER_ERROR("{} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("all animation checks passed");
    return 0;
}
