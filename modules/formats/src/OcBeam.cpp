#include "aver/formats/OcBeam.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <filesystem>

namespace aver::fmt {
using namespace aver::fmt::detail;

const char* beamBehaviorName(BeamBehavior b) {
    switch (b) {
        case BeamBehavior::Deform:   return "Deform";
        case BeamBehavior::Fracture: return "Fracture";
        case BeamBehavior::Shatter:  return "Shatter";
    }
    return "Deform";
}

namespace {

enum class Mode { None, Material, Node, Beam, Panel, Part, Skip };

// A section/directive keyword matches only if the following char is not alphanumeric
// (so a material named "Panelx" doesn't get mistaken for the PANEL section).
bool isKeyword(std::string_view line, std::string_view kw) {
    if (!startsWithCI(line, kw)) return false;
    if (line.size() == kw.size()) return true;
    const char c = line[kw.size()];
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return !alnum;
}

BeamBehavior parseBehavior(std::string_view s) {
    s = trim(s);
    if (equalsCI(s, "SHATTER") || equalsCI(s, "GLASS")) return BeamBehavior::Shatter;
    if (equalsCI(s, "FRACTURE")) return BeamBehavior::Fracture;
    return BeamBehavior::Deform;
}

// Split "ID(a,b,c)" -> id + inner fields. Returns false if no '('.
bool splitIdParen(std::string_view row, i32& id, std::vector<std::string_view>& inner) {
    const usize lp = row.find('(');
    if (lp == std::string_view::npos) return false;
    id = parseI32(trim(row.substr(0, lp)));
    std::string_view rest = row.substr(lp + 1);
    const usize rp = rest.rfind(')');
    if (rp != std::string_view::npos) rest = rest.substr(0, rp);
    inner = splitChar(rest, ',');
    return true;
}

void parsePartRow(std::string_view row, OcBeamData& out) {
    const usize lp = row.find('(');
    if (lp == std::string_view::npos) { out.skippedRows++; return; }
    std::string_view inner = row.substr(lp + 1);
    const usize rp = inner.rfind(')');
    if (rp != std::string_view::npos) inner = inner.substr(0, rp);

    // Separate the optional [panel,list] from the header.
    std::string_view header = inner, listPart;
    const usize lb = inner.find('[');
    if (lb != std::string_view::npos) {
        header = inner.substr(0, lb);
        std::string_view l = inner.substr(lb + 1);
        const usize rb = l.find(']');
        listPart = (rb != std::string_view::npos) ? l.substr(0, rb) : l;
    }

    OcBeamPart part;
    std::vector<std::string_view> positional;
    for (std::string_view tokRaw : splitChar(header, ',')) {
        std::string_view tok = trim(tokRaw);
        if (tok.empty()) continue;
        if (startsWithCI(tok, "DETACH=")) {
            part.detach = static_cast<f32>(parseF64(tok.substr(7), 0.3));
        } else {
            positional.push_back(tok);
        }
    }
    if (positional.size() < 3) { out.skippedRows++; return; }
    part.name = std::string(positional[0]);
    part.role = std::string(positional[1]);
    part.material = std::string(positional[2]);
    if (positional.size() > 3) part.meshName = std::string(positional[3]);

    if (!listPart.empty()) {
        for (std::string_view p : splitChar(listPart, ',')) {
            p = trim(p);
            if (!p.empty()) part.panels.push_back(parseI32(p));
        }
    }
    out.parts.push_back(std::move(part));
}

} // namespace

bool parseOcbeam(std::string_view text, OcBeamData& out, std::string* err) {
    out = OcBeamData{};

    Mode mode = Mode::None;
    f32 scaleDirective = 1.0f;
    bool hasNormalize = false;
    int normAxis = 0; // 0=LENGTH,1=WIDTH,2=HEIGHT
    f32 normCm = 0;

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view raw = text.substr(pos, nl - pos);
        pos = nl + 1;

        std::string_view line = stripTrailingSemicolon(raw);
        if (line.empty()) continue;
        if (line.front() == '#') continue; // .ocbeam: '#' only at line start = comment

        // Lone '}' closes the current section / skip-block.
        if (line == "}") { mode = Mode::None; continue; }

        // Inside a blanket skip-block (BONE/SKIN/ANIM), ignore everything until '}'.
        if (mode == Mode::Skip) continue;

        // --- Header directives (space-delimited) ---
        if (isKeyword(line, "OCBEAM")) { auto t = splitWhitespace(line); if (t.size() > 1) out.version = parseI32(t[1], 1); continue; }
        if (isKeyword(line, "OBJECTID")) { auto t = splitWhitespace(line); if (t.size() > 1) out.objectId = parseU64(t[1]); continue; }
        if (isKeyword(line, "REBOUND")) { auto t = splitWhitespace(line); if (t.size() > 1) { f32 r = static_cast<f32>(parseF64(t[1], -1.0)); out.rebound = r < 0 ? -1.0f : (r > 1 ? 1.0f : r); } continue; }
        if (isKeyword(line, "SCALE")) { auto t = splitWhitespace(line); if (t.size() > 1) scaleDirective = static_cast<f32>(parseF64(t[1], 1.0)); continue; }
        if (isKeyword(line, "NORMALIZE")) {
            auto t = splitWhitespace(line);
            if (t.size() > 2) {
                hasNormalize = true;
                if (equalsCI(t[1], "WIDTH")) normAxis = 1; else if (equalsCI(t[1], "HEIGHT")) normAxis = 2; else normAxis = 0;
                normCm = static_cast<f32>(parseF64(t[2], 0.0));
            }
            continue;
        }
        if (isKeyword(line, "GLBXFORM")) continue; // import-time orientation; not needed at load

        // --- Section keywords ---
        if (isKeyword(line, "MATERIAL")) { mode = Mode::Material; continue; }
        if (isKeyword(line, "NODE"))     { mode = Mode::Node; continue; }
        if (isKeyword(line, "BEAM"))     { mode = Mode::Beam; continue; }
        if (isKeyword(line, "PANEL"))    { mode = Mode::Panel; continue; }
        if (isKeyword(line, "PART"))     { mode = Mode::Part; continue; }
        if (isKeyword(line, "GLB"))      { out.hasEmbeddedGlb = true; mode = Mode::None; continue; } // GLB{}/ENC/B... ignored
        if (isKeyword(line, "BONE") || isKeyword(line, "SKIN") || isKeyword(line, "ANIM")) { out.hasRig = true; mode = Mode::Skip; continue; }
        if (isKeyword(line, "COLLISION") || isKeyword(line, "HULL")) { out.hasCollision = true; mode = Mode::None; continue; }

        // --- Data rows for the current mode ---
        switch (mode) {
            case Mode::Material: {
                auto f = splitChar(line, ',');
                // Accept 10-13 fields: 0-9 (Name..Behavior) are canonical in both the
                // older 10-field and newer 13-field writers; 10-12 are optional extras.
                if (f.size() < 10 || f.size() > 13) { out.skippedRows++; break; }
                OcBeamMaterial m;
                m.name = std::string(trim(f[0]));
                m.stiffness        = static_cast<f32>(parseF64(f[1], 0.7));
                m.axialStiffness   = static_cast<f32>(parseF64(f[2], 3500));
                m.bendForceN       = static_cast<f32>(parseF64(f[3], 3000));
                m.breakForceN      = static_cast<f32>(parseF64(f[4], 12000));
                m.plasticStiffness = static_cast<f32>(parseF64(f[5], 800));
                m.maxBend          = static_cast<f32>(parseF64(f[6], 8));
                m.bendAbsorb       = static_cast<f32>(parseF64(f[7], 0.3));
                m.breakAbsorb      = static_cast<f32>(parseF64(f[8], 0.6));
                m.behavior         = parseBehavior(f[9]);
                if (f.size() >= 11) m.tearStrainTension     = static_cast<f32>(parseF64(f[10], -1.0));
                if (f.size() >= 12) m.tearStrainCompression = static_cast<f32>(parseF64(f[11], -1.0));
                if (f.size() >= 13) m.density               = static_cast<f32>(parseF64(f[12], 1.0));
                out.materials.push_back(std::move(m));
                break;
            }
            case Mode::Node: {
                auto f = splitChar(line, ',');
                if (f.size() != 4) { out.skippedRows++; break; }
                OcBeamNode n;
                n.id = parseI32(f[0]);
                n.x = static_cast<f32>(parseF64(f[1]));
                n.y = static_cast<f32>(parseF64(f[2]));
                n.z = static_cast<f32>(parseF64(f[3]));
                out.nodes.push_back(n);
                break;
            }
            case Mode::Beam: {
                i32 id; std::vector<std::string_view> inner;
                if (!splitIdParen(line, id, inner) || inner.size() < 2) { out.skippedRows++; break; }
                out.beams.push_back({id, parseI32(inner[0]), parseI32(inner[1])});
                break;
            }
            case Mode::Panel: {
                i32 id; std::vector<std::string_view> inner;
                if (!splitIdParen(line, id, inner) || inner.size() < 3) { out.skippedRows++; break; }
                OcBeamPanel p;
                p.id = id;
                p.beamA = parseI32(inner[0]); p.beamB = parseI32(inner[1]); p.beamC = parseI32(inner[2]);
                if (inner.size() >= 4) p.materialOverride = std::string(trim(inner[3]));
                out.panels.push_back(std::move(p));
                break;
            }
            case Mode::Part:
                parsePartRow(line, out);
                break;
            default:
                break; // Mode::None — unknown line, ignored (matches the tolerant runtime)
        }
    }

    // Apply import scale (SCALE or NORMALIZE) to node positions, like the factory.
    f32 factor = scaleDirective;
    if (hasNormalize && !out.nodes.empty()) {
        f32 minX = out.nodes[0].x, maxX = minX, minY = out.nodes[0].y, maxY = minY, minZ = out.nodes[0].z, maxZ = minZ;
        for (const auto& n : out.nodes) {
            minX = n.x < minX ? n.x : minX; maxX = n.x > maxX ? n.x : maxX;
            minY = n.y < minY ? n.y : minY; maxY = n.y > maxY ? n.y : maxY;
            minZ = n.z < minZ ? n.z : minZ; maxZ = n.z > maxZ ? n.z : maxZ;
        }
        const f32 ex = maxX - minX, ey = maxY - minY, ez = maxZ - minZ;
        f32 chosen = ez;
        if (normAxis == 0) chosen = ex > ey ? ex : ey;       // LENGTH = max(X,Y)
        else if (normAxis == 1) chosen = ex < ey ? ex : ey;  // WIDTH  = min(X,Y)
        if (chosen > 1e-6f && normCm > 0) factor = normCm / chosen;
    }
    if (factor != 1.0f) {
        for (auto& n : out.nodes) { n.x *= factor; n.y *= factor; n.z *= factor; }
    }
    out.importScale = factor;

    if (out.nodes.empty()) {
        if (err) *err = "no NODE rows (an .ocbeam requires at least one node)";
        return false;
    }
    return true;
}

bool loadOcbeam(const std::string& path, OcBeamData& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "cannot read file: " + path;
        return false;
    }
    if (!parseOcbeam(text, out, err)) return false;
    if (out.objectId == kInvalidObjectId) {
        out.objectId = makeObjectId(std::filesystem::path(path).stem().string());
    }
    return true;
}

} // namespace aver::fmt
