// .ocinput format test: named input actions and their key/mouse/gamepad bindings (see
// modules/formats/include/aver/formats/OcInput.hpp for the grammar and why each field exists).
// Covers value parsing, round-tripping in memory and on disk, unknown-record and comment
// preservation through a merge write, strict malformed-input rejection, forward-referenced BIND
// records, the two gamepadbutton/gamepadaxis BIND sources, the "commit only on success" contract,
// and OcProject's new INPUT.SCHEME key -- the same shape tests/formats/src/OcParticleTest.cpp uses
// for .ocparticle, since this format's strictness contract is explicitly that precedent (see
// OcInput.hpp's own comment) rather than .ocmat's looser tolerant-default one.
#include "aver/formats/OcInput.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace aver;

static int g_failures = 0;

// Logs one assertion and counts the failures.
static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

static bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// A full .ocinput exercising every record and every BIND source: a first-person "on foot" scheme --
// WASD move on two axes, mouse look, wheel zoom, a mouse-button fire.
static const char* kFull = R"(OCINPUT 1
# A first-person "on foot" scheme: WASD move, mouse look, wheel zoom, left-click fire.
NAME OnFootDefaults

ACTION Move axis2
ACTION Look axis2
ACTION Zoom axis1
ACTION Fire digital

BIND Move key W component y
BIND Move key S scale -1 component y
BIND Move key D component x
BIND Move key A scale -1 component x
BIND Look mousex scale 0.5
BIND Look mousey scale 0.5 component y
BIND Zoom wheel scale 2
BIND Fire key MouseLeft

CONTEXT OnFoot priority 5
)";

// Parses kFull and checks every record's VALUES, not merely that parsing returned true.
static void testFullParse() {
    AVER_INFO("=== .ocinput: every implemented record ===");
    using namespace fmt;

    OcInputData d;
    std::string err;
    if (!parseOcinput(kFull, d, &err)) {
        AVER_ERROR("   parse failed: {}", err);
        ++g_failures;
        return;
    }

    check(d.version == 1, "version is 1");
    check(d.name == "OnFootDefaults", "NAME is parsed");
    check(d.actions.size() == 4, "4 ACTION records reached OcInputData::actions");
    check(d.bindings.size() == 8, "8 BIND records reached OcInputData::bindings");

    if (d.actions.size() == 4) {
        check(d.actions[0].name == "Move" && d.actions[0].type == OcInputValueType::Axis2D,
              "Move is an axis2 action");
        check(d.actions[1].name == "Look" && d.actions[1].type == OcInputValueType::Axis2D,
              "Look is an axis2 action");
        check(d.actions[2].name == "Zoom" && d.actions[2].type == OcInputValueType::Axis1D,
              "Zoom is an axis1 action");
        check(d.actions[3].name == "Fire" && d.actions[3].type == OcInputValueType::Digital,
              "Fire is a digital action");
    }

    if (d.bindings.size() == 8) {
        const OcInputBinding& b0 = d.bindings[0];
        check(b0.action == "Move" && b0.source == OcInputSource::Key && b0.key == "W",
              "BIND 0: Move <- key W");
        check(near(b0.scale, 1.0f), "...scale defaults to 1.0 when the line omits it");
        check(b0.component == 1, "...component y read as 1");

        const OcInputBinding& b1 = d.bindings[1];
        check(b1.action == "Move" && b1.key == "S" && near(b1.scale, -1.0f) && b1.component == 1,
              "BIND 1: Move <- key S, scale -1, component y (the opposing half of a BindAxis1D pair)");

        const OcInputBinding& b2 = d.bindings[2];
        check(b2.key == "D" && b2.component == 0, "BIND 2: component x reads as 0 (explicitly stated)");

        const OcInputBinding& b3 = d.bindings[3];
        check(b3.key == "A" && near(b3.scale, -1.0f) && b3.component == 0,
              "BIND 3: Move <- key A, scale -1, component x");

        const OcInputBinding& b4 = d.bindings[4];
        check(b4.action == "Look" && b4.source == OcInputSource::MouseX && b4.key.empty(),
              "BIND 4: Look <- mousex, no key name for a mouse-axis source");
        check(near(b4.scale, 0.5f) && b4.component == 0, "...scale 0.5, component defaults to x");

        const OcInputBinding& b5 = d.bindings[5];
        check(b5.source == OcInputSource::MouseY && near(b5.scale, 0.5f) && b5.component == 1,
              "BIND 5: Look <- mousey, component y");

        const OcInputBinding& b6 = d.bindings[6];
        check(b6.action == "Zoom" && b6.source == OcInputSource::MouseWheel && near(b6.scale, 2.0f),
              "BIND 6: Zoom <- wheel, scale 2");

        const OcInputBinding& b7 = d.bindings[7];
        check(b7.action == "Fire" && b7.source == OcInputSource::Key && b7.key == "MouseLeft",
              "BIND 7: Fire <- key MouseLeft -- a mouse BUTTON is a Key source, not a fifth OcInputSource "
              "(see OcInput.hpp's own comment on why)");
    }

    check(d.contextName == "OnFoot" && d.contextPriority == 5, "CONTEXT names OnFoot at priority 5");
}

// A header-only file loads as OcInputData's own defaults -- nothing invented, nothing crashed.
static void testHeaderOnlyDefaults() {
    AVER_INFO("=== .ocinput: header-only file loads as defaults ===");
    fmt::OcInputData d;
    std::string err;
    check(fmt::parseOcinput("OCINPUT 1\n", d, &err), "a header-only file parses: " + err);

    const fmt::OcInputData def{};
    check(d.name == def.name, "default name is empty");
    check(d.actions.empty(), "default actions is empty");
    check(d.bindings.empty(), "default bindings is empty");
    check(d.contextName.empty(), "default contextName is empty -- CONTEXT record was absent");
    check(d.contextPriority == 0, "default contextPriority is 0");
}

// Round-trips a scheme built entirely in memory: write (fresh, no `existing`) -> parse -> write must
// reproduce the first write byte for byte, and every field must survive.
static void testRoundTripInMemory() {
    AVER_INFO("=== .ocinput round-trip (in memory, fresh write) ===");
    using namespace fmt;

    OcInputData d;
    d.name = "Vehicle";
    d.actions.push_back({"Throttle", OcInputValueType::Axis1D});
    d.actions.push_back({"Handbrake", OcInputValueType::Digital});

    OcInputBinding accel; accel.action = "Throttle"; accel.source = OcInputSource::Key; accel.key = "W";
    OcInputBinding brake; brake.action = "Throttle"; brake.source = OcInputSource::Key; brake.key = "S";
    brake.scale = -1.0f;
    OcInputBinding hb; hb.action = "Handbrake"; hb.source = OcInputSource::Key; hb.key = "Space";
    d.bindings = {accel, brake, hb};
    d.contextName = "Driving";
    d.contextPriority = 10;

    const std::string text1 = writeOcinput(d);
    check(!text1.empty(), "write produces non-empty text");

    OcInputData d2;
    std::string err;
    check(parseOcinput(text1, d2, &err), "parsed output round-trips: " + err);

    const std::string text2 = writeOcinput(d2);
    check(text1 == text2, "second write reproduces the first byte for byte");

    check(d2.name == "Vehicle", "NAME survived round-trip");
    check(d2.actions.size() == 2 && d2.actions[0].type == OcInputValueType::Axis1D,
          "actions survived round-trip");
    check(d2.bindings.size() == 3 && near(d2.bindings[1].scale, -1.0f),
          "bindings, including a negated scale, survived round-trip");
    check(d2.contextName == "Driving" && d2.contextPriority == 10, "CONTEXT survived round-trip");
}

// The literal requirement: write, save, load, save again produces byte-identical output for a file
// this module produced -- exercised through real disk I/O, not just in-memory strings. There is no
// saveOcinput (this format mirrors OcProject's three-function shape, not OcGraph/OcParticle's five --
// see OcInput.hpp), so the test writes the file itself, exactly as an editor calling writeOcinput
// would.
static void testFileRoundTrip() {
    AVER_INFO("=== .ocinput round-trip (real file) ===");
    using namespace fmt;
    const std::string dir = std::getenv("TEMP") ? std::getenv("TEMP") : ".";
    const std::string path = dir + "/aver-ocinput-test-roundtrip.ocinput";

    OcInputData d;
    d.name = "Menu";
    d.actions.push_back({"Confirm", OcInputValueType::Digital});
    OcInputBinding b; b.action = "Confirm"; b.source = OcInputSource::Key; b.key = "Enter";
    d.bindings.push_back(b);
    d.contextName = "UI";

    const std::string text1 = writeOcinput(d);
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << text1;
    }

    std::string onDisk1;
    check(readFileText(path, onDisk1), "the saved file is readable");
    check(onDisk1 == text1, "what is on disk matches what writeOcinput produced");

    OcInputData d2;
    std::string err;
    check(loadOcinput(path, d2, &err), "load of the just-saved file succeeds: " + err);
    check(d2.name == "Menu" && d2.bindings.size() == 1, "the loaded scheme carries the same data");

    const std::string text2 = writeOcinput(d2, onDisk1);
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << text2;
    }
    std::string onDisk2;
    check(readFileText(path, onDisk2), "the re-saved file is readable");
    check(onDisk1 == onDisk2, "write -> save -> load -> merge-write -> save produces a byte-identical file");
}

// Unknown records and comments must survive a save -- the requirement this format takes from
// .ocgraph/.ocparticle (see OcInput.hpp's writeOcinput comment), not from .ocmat (which drops its own
// GRAPH{} block on rewrite and warns about it).
static void testUnknownRecordsAndComments() {
    AVER_INFO("=== .ocinput: unknown records and comments survive a save ===");
    using namespace fmt;

    const std::string original =
        "OCINPUT 1\n"
        "NAME Original\n"
        "\n"
        "ACTION Move axis2\n"
        "ACTION Fire digital\n"
        "\n"
        "BIND Move key W component y\n"
        "BIND Move key S scale -1 component y\n"
        "BIND Fire key MouseLeft\n"
        "\n"
        "CONTEXT OnFoot priority 0\n"
        "# A hand-written note about why Fire uses MouseLeft\n"
        "MYSTERY some future field nobody reads yet\n";

    OcInputData d;
    std::string err;
    check(parseOcinput(original, d, &err), "a scheme with an unknown record parses: " + err);
    check(d.name == "Original", "known fields are parsed");

    const std::string rewritten = writeOcinput(d, original);
    check(rewritten.find("MYSTERY some future field nobody reads yet") != std::string::npos,
          "the unknown record 'MYSTERY' survives a save");
    check(rewritten.find("# A hand-written note about why Fire uses MouseLeft") != std::string::npos,
          "the comment survives a save");
    check(rewritten == original, "an unmodified merge reproduces the file byte for byte");

    OcInputData d2;
    check(parseOcinput(rewritten, d2, &err), "the rewritten scheme still parses: " + err);

    const std::string again = writeOcinput(d2, rewritten);
    check(again.find("MYSTERY some future field nobody reads yet") != std::string::npos,
          "the unknown record survives a SECOND save");
    check(again == rewritten, "the second merge is idempotent -- bit-identical to the first");
}

// gamepadbutton and gamepadaxis: the two BIND sources added beside key/mousex/mousey/wheel, so a
// scheme can bind a pad the same way it binds a keyboard. Round-trips byte-identical, same
// requirement testRoundTripInMemory already states for key.
static void testGamepadBindings() {
    AVER_INFO("=== .ocinput: gamepadbutton and gamepadaxis round-trip byte-identically ===");
    using namespace fmt;

    OcInputData d;
    d.name = "Pad";
    d.actions.push_back({"Fire", OcInputValueType::Digital});
    d.actions.push_back({"Move", OcInputValueType::Axis1D});

    OcInputBinding fire;
    fire.action = "Fire"; fire.source = OcInputSource::GamepadButton; fire.key = "RightShoulder";
    OcInputBinding move;
    move.action = "Move"; move.source = OcInputSource::GamepadAxis; move.key = "LeftTrigger";
    move.scale = -1.0f; move.component = 2;
    d.bindings = {fire, move};

    const std::string text1 = writeOcinput(d);
    check(text1.find("BIND Fire gamepadbutton RightShoulder\n") != std::string::npos,
          "a gamepadbutton BIND writes its button name, with no scale/component when they hold "
          "their defaults");
    check(text1.find("BIND Move gamepadaxis LeftTrigger scale -1 component z\n") != std::string::npos,
          "a gamepadaxis BIND writes its axis name alongside scale and component");

    OcInputData d2;
    std::string err;
    check(parseOcinput(text1, d2, &err), "the written gamepad bindings parse back: " + err);
    check(d2.bindings.size() == 2, "both gamepad bindings survived the round trip");
    if (d2.bindings.size() == 2) {
        check(d2.bindings[0].source == OcInputSource::GamepadButton && d2.bindings[0].key == "RightShoulder",
              "BIND 0: Fire <- gamepadbutton RightShoulder");
        check(d2.bindings[1].source == OcInputSource::GamepadAxis && d2.bindings[1].key == "LeftTrigger" &&
              near(d2.bindings[1].scale, -1.0f) && d2.bindings[1].component == 2,
              "BIND 1: Move <- gamepadaxis LeftTrigger, scale -1, component z");
    }

    const std::string text2 = writeOcinput(d2);
    check(text1 == text2, "second write reproduces the first byte for byte");
}

// A malformed gamepadbutton/gamepadaxis BIND (no name token) is rejected the same way a bare `key`
// with no key name already is -- see testMalformedInput's own "BIND key with no key name" case.
static void testMalformedGamepadBindings() {
    AVER_INFO("=== .ocinput: malformed gamepad BIND records are rejected ===");
    fmt::OcInputData d;
    std::string err;

    check(!fmt::parseOcinput("OCINPUT 1\nACTION Fire digital\nBIND Fire gamepadbutton\n", d, &err),
          "a gamepadbutton BIND with no button name is rejected");
    check(!fmt::parseOcinput("OCINPUT 1\nACTION Move axis1\nBIND Move gamepadaxis\n", d, &err),
          "a gamepadaxis BIND with no axis name is rejected");

    // What must still fail: an unrelated made-up source word is refused exactly as before -- adding
    // two new sources must not have widened BIND into accepting anything unrecognised.
    check(!fmt::parseOcinput("OCINPUT 1\nACTION Move axis2\nBIND Move joystick 0\n", d, &err),
          "an unknown BIND source is still rejected with gamepad sources added");
}

// Same data, written twice, must produce identical bytes.
static void testDeterministic() {
    AVER_INFO("=== .ocinput: deterministic output ===");
    fmt::OcInputData d;
    d.name = "Deterministic";
    d.actions.push_back({"Jump", fmt::OcInputValueType::Digital});
    fmt::OcInputBinding b; b.action = "Jump"; b.source = fmt::OcInputSource::Key; b.key = "Space";
    d.bindings.push_back(b);

    const std::string t1 = fmt::writeOcinput(d);
    const std::string t2 = fmt::writeOcinput(d);
    check(t1 == t2, "identical schemes produce identical output");
}

// BIND is allowed to name an ACTION declared LATER in the file -- see OcInput.cpp's header comment
// on why BIND names its action explicitly rather than meaning "the action above it", which is what
// makes this legal in the first place.
static void testBindForwardReference() {
    AVER_INFO("=== .ocinput: BIND may forward-reference an ACTION declared later ===");
    fmt::OcInputData d;
    std::string err;
    check(fmt::parseOcinput("OCINPUT 1\nBIND Move key W\nACTION Move axis2\n", d, &err),
          "a BIND before its ACTION still parses: " + err);
    check(d.bindings.size() == 1 && d.bindings[0].action == "Move", "...and the binding is recorded");
    check(d.actions.size() == 1 && d.actions[0].name == "Move", "...alongside the action it names");
}

// Checks which malformed inputs are refused, in memory, and that the version header is enforced.
static void testMalformedInput() {
    AVER_INFO("=== .ocinput: malformed input is rejected ===");
    fmt::OcInputData d;
    std::string err;

    check(!fmt::parseOcinput("NAME Test\n", d, &err), "input without an OCINPUT header is rejected");
    check(err.find("OCINPUT") != std::string::npos, "...error message mentions the missing header");

    check(!fmt::parseOcinput("OCINPUT 3\n", d, &err), "an unknown OCINPUT version is rejected");
    check(!fmt::parseOcinput("OCINPUT 0\n", d, &err), "version 0 is rejected");
    check(fmt::parseOcinput("OCINPUT 2\n", d, &err), "OCINPUT 2 (the clamp modifier's version) is accepted");
    check(!fmt::parseOcinput("OCINPUT\n", d, &err), "a header with no version number is rejected");
    check(!fmt::parseOcinput("", d, &err), "a completely empty file is rejected");
    check(err.find("OCINPUT") != std::string::npos, "...and the empty-file error also names the header");
    check(fmt::parseOcinput("OCINPUT 1\n", d, &err), "OCINPUT 1 itself is accepted");

    check(!fmt::parseOcinput("OCINPUT 1\nACTION Move\n", d, &err),
          "a truncated ACTION (missing type) is rejected");
    check(err.find("ACTION") != std::string::npos, "...naming ACTION");

    check(!fmt::parseOcinput("OCINPUT 1\nACTION Move sometype\n", d, &err),
          "an unknown ACTION type is rejected");

    check(!fmt::parseOcinput("OCINPUT 1\nACTION Move axis2\nBIND Jump key Space\n", d, &err),
          "a BIND naming an ACTION this file never declares is rejected");
    check(err.find("Jump") != std::string::npos, "...and the error names the offending action");

    check(!fmt::parseOcinput("OCINPUT 1\nACTION Move axis2\nBIND Move joystick 0\n", d, &err),
          "an unknown BIND source is rejected");

    check(!fmt::parseOcinput("OCINPUT 1\nACTION Fire digital\nBIND Fire key\n", d, &err),
          "a BIND key with no key name is rejected");

    check(!fmt::parseOcinput("OCINPUT 1\nACTION Move axis2\nBIND Move key W scale notanumber\n", d, &err),
          "a malformed number in BIND scale is rejected, not silently read as zero");

    check(!fmt::parseOcinput("OCINPUT 1\nACTION Move axis2\nBIND Move key W component up\n", d, &err),
          "BIND component must be x, y or z");

    check(!fmt::parseOcinput("OCINPUT 1\nACTION Move axis2\nBIND Move key W scale\n", d, &err),
          "a BIND keyword with no trailing value is rejected");

    check(!fmt::parseOcinput("OCINPUT 1\nACTION Move axis2\nBIND Move key W speed 5\n", d, &err),
          "an unrecognised BIND attribute (not scale or component) is rejected");

    check(!fmt::parseOcinput("OCINPUT 1\nCONTEXT OnFoot 5\n", d, &err),
          "a CONTEXT with too few fields is rejected");
    check(!fmt::parseOcinput("OCINPUT 1\nCONTEXT OnFoot prio 5\n", d, &err),
          "CONTEXT missing the literal 'priority' keyword is rejected");
    check(!fmt::parseOcinput("OCINPUT 1\nCONTEXT OnFoot priority notanumber\n", d, &err),
          "a malformed CONTEXT priority is rejected");

    // What must NOT fail: a record kind this format has never heard of is forward-compat, not an
    // error, matching .ocgraph/.ocparticle's own "unknown records are ignored during parse" rule.
    check(fmt::parseOcinput("OCINPUT 1\nSOMETHING_FUTURE 1 2 3\n", d, &err),
          "an unrecognised record KIND is tolerated, not fatal");
}

// A malformed parse must not leave the caller's struct holding a mix of real records and defaults --
// the same guarantee OcParticle.cpp's parseOcparticle states at length, checked here the same way:
// starting from a struct PRE-POPULATED with values that are not the defaults, where a leaked partial
// parse would be unmistakable.
static void testCommitOnlyOnSuccess() {
    AVER_INFO("=== .ocinput: a failed parse leaves the caller's struct at defaults ===");
    fmt::OcInputData d;
    d.name = "sentinel";
    d.actions.push_back({"Sentinel", fmt::OcInputValueType::Axis2D});
    d.contextName = "SentinelContext";
    d.contextPriority = 999;

    std::string err;
    const bool ok = fmt::parseOcinput("OCINPUT 1\nACTION Move axis2\nBIND Move key W scale bogus\n", d, &err);
    check(!ok, "the deliberately malformed text is rejected");

    const fmt::OcInputData def{};
    check(d.name == def.name, "name is reset to the default, not left at 'sentinel'");
    check(d.actions.empty(), "actions is reset to empty, not left holding the sentinel action");
    check(d.contextName.empty() && d.contextPriority == 0,
          "CONTEXT fields are reset too, not left at the sentinel's values");
}

// OcProject's new INPUT.SCHEME key: parses, round-trips, and leaves every other key untouched --
// the same discipline FormatTest.cpp's checkOcproject already verifies for DRONE.GRAPH, applied to
// the key this change adds beside it.
static void testOcProjectInputScheme() {
    AVER_INFO("=== OcProject INPUT.SCHEME (names this project's default .ocinput) ===");
    using namespace fmt;

    const std::string original =
        "OCPROJECT 1\n"
        "NAME SkyForge\n"
        "ENGINE Aver 0.1.0\n"
        "CONTENT Content\n"
        "STARTMAP Maps/Default.ocworld\n"
        "DRONE.GRAPH Graphs/Drone.ocgraph\n";

    ProjectDesc d;
    std::string err;
    check(parseOcproject(original, d, &err), "the manifest parses");
    check(d.inputScheme.empty(), "a manifest with no INPUT.SCHEME leaves it empty, not a guessed path");

    d.inputScheme = "Input/OnFoot.ocinput";
    const std::string written = writeOcproject(d, original);
    check(written.find("INPUT.SCHEME Input/OnFoot.ocinput") != std::string::npos,
          "setting inputScheme in memory adds an INPUT.SCHEME line on the next save");
    check(written.find("DRONE.GRAPH Graphs/Drone.ocgraph") != std::string::npos,
          "...without disturbing DRONE.GRAPH");
    check(written.find("NAME SkyForge") != std::string::npos &&
          written.find("STARTMAP Maps/Default.ocworld") != std::string::npos,
          "...or NAME/STARTMAP, or any other existing key");

    ProjectDesc back;
    check(parseOcproject(written, back, &err), "what was written parses again");
    check(back.inputScheme == "Input/OnFoot.ocinput", "and the new key round-trips");
    check(back.droneGraph == "Graphs/Drone.ocgraph" && back.name == "SkyForge",
          "alongside the keys that predate it");

    const std::string again = writeOcproject(back, written);
    check(again == written, "re-saving an unchanged manifest with INPUT.SCHEME set is byte-stable");

    // Clearing it is the mirror image of DRONE.GRAPH's own contract (OcProject.hpp's comment on that
    // field): an empty inputScheme removes the line rather than writing a blank one.
    back.inputScheme.clear();
    const std::string cleared = writeOcproject(back, again);
    check(cleared.find("INPUT.SCHEME") == std::string::npos,
          "clearing inputScheme removes the line entirely");
    check(cleared.find("DRONE.GRAPH Graphs/Drone.ocgraph") != std::string::npos,
          "...without disturbing DRONE.GRAPH or anything else");
}

// Trigger and modifier specs: every kind's exact fields, canonical text both ways, named rejections,
// attachment to ACTION/BIND in either order, the declared-action post-pass, and merge-write fidelity.
static void testTriggerModifierGrammar() {
    AVER_INFO("=== .ocinput: trigger and modifier specs ===");
    using namespace fmt;
    std::string err;

    OcTriggerSpec t;
    check(parseTriggerSpec("hold:0.5:0.3:once", t, &err) && t.kind == OcTriggerKind::Hold &&
          near(t.time, 0.5f) && near(t.actuation, 0.3f) && t.once, "hold:0.5:0.3:once");
    check(parseTriggerSpec("HOLD:once:0.5", t, &err) && near(t.time, 0.5f) && t.once &&
          near(t.actuation, 0.15f), "flags may sit anywhere and keywords are case-insensitive");
    check(parseTriggerSpec("pulse:0.25:3:nostart", t, &err) && t.kind == OcTriggerKind::Pulse &&
          near(t.time, 0.25f) && t.limit == 3 && !t.onStart, "pulse:0.25:3:nostart");
    check(parseTriggerSpec("pulse:0.12", t, &err) && t.limit == 0 && t.onStart, "pulse defaults: unlimited, onstart");
    check(parseTriggerSpec("tap", t, &err) && near(t.time, 0.2f), "tap defaults to max 0.2");
    check(parseTriggerSpec("holdrelease:1:0.4", t, &err) && t.kind == OcTriggerKind::HoldRelease &&
          near(t.time, 1.0f) && near(t.actuation, 0.4f), "holdrelease:1:0.4");
    check(parseTriggerSpec("chord:Mod", t, &err) && t.kind == OcTriggerKind::Chord && t.refs.size() == 1 &&
          t.refs[0] == "Mod", "chord keeps the action name's case");
    check(parseTriggerSpec("combo:0.4:Fire,Fire,Jump", t, &err) && t.kind == OcTriggerKind::Combo &&
          near(t.time, 0.4f) && t.refs.size() == 3 && t.refs[2] == "Jump", "combo:0.4:Fire,Fire,Jump");
    check(parseTriggerSpec("released", t, &err) && t.kind == OcTriggerKind::Released, "released");

    OcModifierSpec m;
    check(parseModifierSpec("deadzone:0.1:0.9:axial", m, &err) && m.kind == OcModifierKind::DeadZone &&
          near(m.v[0], 0.1f) && near(m.v[1], 0.9f) && m.mode == 1, "deadzone:0.1:0.9:axial");
    check(parseModifierSpec("deadzone", m, &err) && near(m.v[0], 0.2f) && near(m.v[1], 1.0f) && m.mode == 0,
          "deadzone defaults: 0.2, 1, radial");
    check(parseModifierSpec("negate:xz", m, &err) && m.kind == OcModifierKind::Negate && m.mask == 5, "negate:xz is mask 5");
    check(parseModifierSpec("negate", m, &err) && m.mask == 7, "bare negate flips all three channels");
    check(parseModifierSpec("swizzle:zyx", m, &err) && m.kind == OcModifierKind::Swizzle && m.mode == 2, "swizzle:zyx is mode 2");
    check(parseModifierSpec("scale:2", m, &err) && near(m.v[0], 2) && near(m.v[1], 2) && near(m.v[2], 2),
          "a single scale value is uniform");
    check(parseModifierSpec("scale:2:3", m, &err) && near(m.v[1], 3) && near(m.v[2], 1), "scale:x:y takes z = 1");
    check(parseModifierSpec("curve:2", m, &err) && m.kind == OcModifierKind::Curve && near(m.v[2], 2),
          "a single curve exponent is uniform");
    check(parseModifierSpec("smooth:0.04", m, &err) && m.kind == OcModifierKind::Smooth && near(m.v[0], 0.04f), "smooth:0.04");
    check(parseModifierSpec("clamp", m, &err) && m.kind == OcModifierKind::Clamp && near(m.v[0], 0.0f) && near(m.v[1], 1.0f), "clamp defaults to 0..1");
    check(parseModifierSpec("clamp:-1:0", m, &err) && near(m.v[0], -1.0f) && near(m.v[1], 0.0f), "clamp:-1:0");
    check(parseModifierSpec("clamp:0.25", m, &err) && near(m.v[0], 0.25f) && near(m.v[1], 1.0f), "clamp:0.25 takes the upper bound 1");

    // parse(format(x)) == x and format(parse(s)) == s over canonical text.
    const char* const kTriggers[] = {
        "down", "down:0.3", "pressed", "pressed:0.5", "released", "released:0.25", "hold:0.5", "hold:0.5:0.3",
        "hold:0.5:once", "hold:0.5:0.3:once", "hold:0", "holdrelease:1", "holdrelease:1:0.4", "tap", "tap:0.18",
        "tap:0.18:0.3", "pulse:0.12", "pulse:0.12:3", "pulse:0.25:3:nostart", "pulse:0.12:0:0.3",
        "pulse:0.12:nostart", "chord:Mod", "combo:0.4:Fire,Fire,Jump"};
    const char* const kModifiers[] = {
        "deadzone", "deadzone:0.1", "deadzone:0.1:0.9", "deadzone:0.1:0.9:axial", "deadzone:axial", "scale:2",
        "scale:1:2:3", "negate", "negate:y", "negate:xz", "swizzle:yxz", "swizzle:zxy", "smooth:0.04",
        "smooth:0", "curve:2", "curve:1.4:2:1", "clamp", "clamp:-1:0", "clamp:0.25", "clamp:0:0.5"};
    bool allCanonical = true, allRound = true;
    for (const char* s : kTriggers) {
        OcTriggerSpec a, b;
        if (!parseTriggerSpec(s, a, &err)) { allCanonical = false; AVER_ERROR("   trigger '{}' rejected: {}", s, err); continue; }
        if (formatTriggerSpec(a) != s) { allCanonical = false; AVER_ERROR("   trigger '{}' formats as '{}'", s, formatTriggerSpec(a)); }
        if (!parseTriggerSpec(formatTriggerSpec(a), b, &err) || !(a == b)) allRound = false;
    }
    for (const char* s : kModifiers) {
        OcModifierSpec a, b;
        if (!parseModifierSpec(s, a, &err)) { allCanonical = false; AVER_ERROR("   modifier '{}' rejected: {}", s, err); continue; }
        if (formatModifierSpec(a) != s) { allCanonical = false; AVER_ERROR("   modifier '{}' formats as '{}'", s, formatModifierSpec(a)); }
        if (!parseModifierSpec(formatModifierSpec(a), b, &err) || !(a == b)) allRound = false;
    }
    check(allCanonical, "every canonical spec formats back to itself");
    check(allRound, "parse(format(x)) == x for every spec");

    // Rejections carry a message naming the problem.
    const char* const badTriggers[] = {
        "hold:-1", "hold", "hold:0.5:", "hold 0.5", "hold:0.5:flag", "bogus", "tap:-0.1", "pulse:0", "pulse:0.1:1.5",
        "pulse:0.1:onstart:nostart", "combo:0.4:Solo", "combo:0:A,B", "combo:0.4:A,,B", "chord:", "chord:A,B",
        "down:-1", "hold:0.5:0.1:0.2", "hold:0.5:once:once", "pressed#x", "", "hold:nan"};
    bool allRejected = true;
    for (const char* s : badTriggers) {
        OcTriggerSpec x; err.clear();
        if (parseTriggerSpec(s, x, &err) || err.empty()) { allRejected = false; AVER_ERROR("   trigger '{}' was not rejected with a message", s); }
    }
    const char* const badModifiers[] = {
        "deadzone:0.5:0.5", "deadzone:0.9:0.1", "deadzone:-1", "deadzone:radial:axial", "scale", "curve:0", "curve:-2",
        "negate:xq", "negate:x:y", "swizzle", "swizzle:xxx", "smooth", "smooth:-1", "nope", "scale:1:", "scale 2",
        "clamp:1:0", "clamp:0.5:0.5", "clamp:0:1:2", "clamp:x"};
    for (const char* s : badModifiers) {
        OcModifierSpec x; err.clear();
        if (parseModifierSpec(s, x, &err) || err.empty()) { allRejected = false; AVER_ERROR("   modifier '{}' was not rejected with a message", s); }
    }
    check(allRejected, "every malformed spec is rejected with a named error");
    {
        OcTriggerSpec keep; keep.kind = OcTriggerKind::Tap; keep.time = 0.33f;
        OcTriggerSpec x = keep;
        parseTriggerSpec("hold:-1", x, &err);
        check(x == keep, "a rejected spec leaves the output untouched");
    }

    // ACTION and BIND carry the pairs, in either order relative to scale/component and to each other.
    OcInputData d;
    const char* const kText =
        "OCINPUT 1\n"
        "ACTION Move axis2 modifier deadzone:0.2:1:radial modifier smooth:0.04\n"
        "ACTION Mod digital\n"
        "ACTION Save digital trigger pressed trigger chord:Mod\n"
        "ACTION Combo1 digital trigger combo:0.4:Fire,Fire,Jump\n"
        "ACTION Fire digital\n"
        "ACTION Jump digital\n"
        "BIND Move gamepadaxis LeftY trigger down modifier negate:y scale -1 component y\n"
        "BIND Move mousex scale 0.5 modifier curve:1.4\n"
        "BIND Jump key Space trigger tap:0.18\n"
        "BIND Save key S\n";
    check(parseOcinput(kText, d, &err), "a scheme with ACTION/BIND pairs parses: " + err);
    if (d.actions.size() == 6 && d.bindings.size() == 4) {
        check(d.actions[0].modifiers.size() == 2 && d.actions[0].modifiers[0] == "deadzone" &&
              d.actions[0].modifiers[1] == "smooth:0.04", "action modifiers are stored canonical, in order");
        check(d.actions[2].triggers.size() == 2 && d.actions[2].triggers[1] == "chord:Mod", "action triggers keep order");
        check(d.bindings[0].triggers.size() == 1 && d.bindings[0].modifiers.size() == 1 &&
              near(d.bindings[0].scale, -1.0f) && d.bindings[0].component == 1,
              "pairs and scale/component mix in any order on a BIND");
        check(d.bindings[2].triggers.size() == 1 && d.bindings[2].triggers[0] == "tap:0.18", "binding trigger");
    } else check(false, "unexpected record counts");

    const std::string written = writeOcinput(d);
    check(written.find("ACTION Move axis2 modifier deadzone modifier smooth:0.04\n") != std::string::npos,
          "the writer emits canonical modifier text after the type");
    check(written.find("BIND Move gamepadaxis LeftY scale -1 component y trigger down modifier negate:y\n") != std::string::npos,
          "the writer emits trigger/modifier pairs after scale/component");
    OcInputData d2;
    check(parseOcinput(written, d2, &err), "the written scheme parses again: " + err);
    check(writeOcinput(d2) == written, "write -> parse -> write is stable");

    // The merge path keeps a hand comment and unknown records while replacing the blocks.
    const std::string withComment = "OCINPUT 1\n# keep me\nACTION Fire digital\nMYSTERY x\nBIND Fire key MouseLeft\n";
    OcInputData d3;
    check(parseOcinput(withComment, d3, &err), "comment fixture parses");
    d3.actions[0].triggers.push_back("hold:0.5");
    const std::string merged = writeOcinput(d3, withComment);
    check(merged.find("# keep me\n") != std::string::npos && merged.find("MYSTERY x\n") != std::string::npos &&
          merged.find("ACTION Fire digital trigger hold:0.5\n") != std::string::npos,
          "merge keeps the comment and the unknown record and rewrites the ACTION line");

    // Post-pass: referenced actions must exist, a chord may not name its own action.
    check(!parseOcinput("OCINPUT 1\nACTION A digital trigger chord:Ghost\n", d, &err) && !err.empty(),
          "a chord naming an undeclared action rejects the file");
    check(!parseOcinput("OCINPUT 1\nACTION A digital\nBIND A key W trigger combo:0.5:A,Ghost\n", d, &err),
          "a combo step naming an undeclared action rejects the file");
    check(!parseOcinput("OCINPUT 1\nACTION A digital trigger chord:A\n", d, &err), "a chord naming its own action rejects the file");
    check(!parseOcinput("OCINPUT 1\nACTION A digital\nBIND A key W trigger chord:A\n", d, &err),
          "...also when the chord sits on one of its bindings");
    check(parseOcinput("OCINPUT 1\nACTION A digital trigger chord:B\nACTION B digital trigger chord:A\n", d, &err),
          "a two-action chord cycle is legal data (the engine reads None for it)");
    check(!parseOcinput("OCINPUT 1\nACTION A digital trigger\n", d, &err), "a trailing keyword with no value is rejected");
    check(!parseOcinput("OCINPUT 1\nACTION A digital color red\n", d, &err), "an unknown ACTION attribute is rejected");
    check(!parseOcinput("OCINPUT 1\nACTION A digital\nBIND A key W trigger hold\n", d, &err) &&
          err.find("BIND trigger:") == 0, "a bad BIND spec's error is prefixed with its record and keyword");

    // No new keyword: the writer is byte-identical to what it was before this feature.
    OcInputData full;
    check(parseOcinput(kFull, full, &err), "kFull parses");
    {
        // The writer has always dropped a default `component x` (kFull spells it out on the D and A bindings).
        std::string want = kFull;
        for (size_t p; (p = want.find(" component x\n")) != std::string::npos;) want.erase(p, 12);
        const std::string back = writeOcinput(full, kFull);
        size_t at = 0;
        while (at < back.size() && at < want.size() && back[at] == want[at]) ++at;
        check(back == want, "a scheme with no triggers/modifiers merges back as before this feature (first difference at " +
                                std::to_string(at) + ")");
    }
    const std::string fresh = writeOcinput(full);
    check(fresh.find("trigger") == std::string::npos && fresh.find("modifier") == std::string::npos &&
          fresh.find("ACTION Move axis2\nACTION Look axis2\n") != std::string::npos,
          "...and a fresh write adds no trigger/modifier text");
    check(fresh.rfind("OCINPUT 1\n", 0) == 0, "...and still says OCINPUT 1");

    // A half axis: scale -1 and clamp keep one direction of a signed axis. A scheme using clamp is version 2,
    // reads back the same, and a version 1 file still loads.
    {
        const std::string half = "OCINPUT 2\nACTION Left digital\nBIND Left gamepadaxis LeftX scale -1 modifier clamp\n";
        OcInputData h;
        check(parseOcinput(half, h, &err), "a version 2 file with a clamp modifier parses: " + err);
        check(h.bindings.size() == 1 && h.bindings[0].modifiers.size() == 1 && h.bindings[0].modifiers[0] == "clamp" && h.bindings[0].scale == -1.0f,
              "the clamp is kept on the binding as canonical text");
        const std::string out = writeOcinput(h);
        check(out.rfind("OCINPUT 2\n", 0) == 0 && out.find("modifier clamp\n") != std::string::npos, "the writer says version 2 for it");
        OcInputData again;
        check(parseOcinput(out, again, &err) && writeOcinput(again) == out, "write -> parse -> write is stable");
        const std::string merged2 = writeOcinput(h, "OCINPUT 1\n# mine\nACTION Left digital\n");
        check(merged2.rfind("OCINPUT 2\n", 0) == 0 && merged2.find("# mine\n") != std::string::npos, "merging into a version 1 file raises the header and keeps the comment");
        OcInputData old;
        check(parseOcinput("OCINPUT 1\nACTION Left digital\nBIND Left key A\n", old, &err), "a version 1 file still loads");
        check(writeOcinput(old).rfind("OCINPUT 1\n", 0) == 0, "and is written back as version 1");
    }
}

int main() {
    testFullParse();
    testHeaderOnlyDefaults();
    testRoundTripInMemory();
    testFileRoundTrip();
    testUnknownRecordsAndComments();
    testDeterministic();
    testBindForwardReference();
    testGamepadBindings();
    testMalformedGamepadBindings();
    testTriggerModifierGrammar();
    testMalformedInput();
    testCommitOnlyOnSuccess();
    testOcProjectInputScheme();

    AVER_INFO("==================================================");
    AVER_INFO("OcInput tests done: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
