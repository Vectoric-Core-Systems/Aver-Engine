// The .ocfonts font set editor tab's headless core: the document edits (free functions), the tab's own
// load / save / dirty / undo bookkeeping, coverage, and the preview engine that draws the unsaved set.
//
// AVER_WITH_IMGUI IS DELIBERATELY UNDEFINED for this target, as for BtEditorTest: FontSetEditor.cpp's drawing half
// and the GPU upload of the atlas pages sit behind that macro, so this exercises exactly the part with no window.
#include "FontSetEditor.hpp"

#include "../../ui/src/TestFont.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/ui/UiFontFace.hpp"
#include "aver/ui/UiFontSet.hpp"

#include <filesystem>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::editor;
using namespace aver::ui;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static UiFontSetDoc parsed(const std::string& text) {
    UiFontSetDoc d;
    std::string why;
    if (!UiFontSet::parse(text, d, &why)) AVER_ERROR("fixture does not parse: {}", why);
    return d;
}

// Entries must stay Project, System, Bitmap.
static bool grouped(const UiFontFamilyDef& f) {
    int last = 0;
    for (const UiFontEntry& e : f.entries) {
        const int g = static_cast<int>(e.source);
        if (g < last) return false;
        last = g;
    }
    return true;
}

static void writeBytes(const std::string& path, const std::vector<u8>& b) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    writeFileBytes(path, b.data(), b.size());
}

int main() {
    AVER_INFO("FontSetEditorTest");

    AVER_INFO("the starter document");
    {
        const UiFontSetDoc d = parsed(fontSetStarterText());
        check(fsValidate(d).empty(), "the starter set validates");
        check(d.families.size() == 1 && d.families[0].name == "Body", "one family, Body");
        check(d.defaultFamily == "Body", "Body is the default");
        check(d.families[0].entries.size() == 4, "two faces, the OS fonts and the bitmap font");
        check(grouped(d.families[0]), "in Project, System, Bitmap order");
        check(UiFontSet::write(d).find("OCFONTS 1") == 0, "it writes back with the header first");
    }

    AVER_INFO("families");
    {
        UiFontSetDoc d;
        check(fsValidate(d) == "the font set needs at least one family", "an empty set is invalid");
        const i32 a = fsAddFamily(d, "Body");
        check(a == 0 && d.defaultFamily == "Body", "the first family becomes the default");
        const i32 b = fsAddFamily(d, "body");
        check(b == 1 && d.families[1].name == "body 2", "a name taken ignoring case is made unique");
        const i32 c = fsAddFamily(d, "");
        check(c == 2 && d.families[2].name == "Family", "an empty name gets a placeholder");
        check(fsValidate(d).empty(), "a set of empty families validates");

        check(!fsRenameFamily(d, 1, "BODY"), "renaming onto another family is refused");
        check(!fsRenameFamily(d, 1, ""), "an empty name is refused");
        check(!fsRenameFamily(d, 9, "X"), "a bad index is refused");
        check(fsRenameFamily(d, 0, "BODY"), "renaming a family to a new case of its own name works");

        check(fsAddOverride(d, "ja", "BODY", "Family") == 0, "an override between two families is added");
        check(fsRenameFamily(d, 0, "Main"), "rename");
        check(d.defaultFamily == "Main", "the default follows a rename");
        check(d.overrides[0].from == "Main", "an override follows a rename of its source");
        check(fsRenameFamily(d, 2, "Cjk"), "rename the target");
        check(d.overrides[0].to == "Cjk", "an override follows a rename of its target");
        check(fsValidate(d).empty(), "still valid");

        check(fsSetDefault(d, 1) && d.defaultFamily == "body 2", "set default");
        check(!fsSetDefault(d, 7), "set default on a bad index is refused");

        check(fsRemoveFamily(d, 2), "remove the override's target family");
        check(d.overrides.empty(), "overrides naming it go with it");
        check(fsRemoveFamily(d, 1), "remove the default family");
        check(d.defaultFamily == "Main", "the default moves to the first family left");
        check(!fsRemoveFamily(d, 0), "the last family cannot be removed");
        check(d.families.size() == 1 && fsValidate(d).empty(), "one family, valid");
    }

    AVER_INFO("entries");
    {
        UiFontSetDoc d;
        fsAddFamily(d, "A");
        const i32 bmp = fsAddBitmap(d, 0);
        check(bmp == 0, "the bitmap font is the only entry");
        check(fsAddBitmap(d, 0) == 0 && d.families[0].entries.size() == 1, "the bitmap font is added once");
        const i32 sys = fsAddSystem(d, 0);
        check(sys == 0 && d.families[0].entries[0].file == "auto", "an OS entry goes before the bitmap font, auto by default");
        const i32 f1 = fsAddFace(d, 0, "Fonts/One.ttf");
        const i32 f2 = fsAddFace(d, 0, "Fonts/Two.ttf");
        check(f1 == 0 && f2 == 1, "project faces go first, in order");
        check(grouped(d.families[0]) && d.families[0].entries.size() == 4, "the chain stays grouped");
        check(fsAddFace(d, 0, "") == -1, "an empty path is refused");
        check(fsAddFace(d, 5, "X.ttf") == -1, "a bad family is refused");

        check(fsMoveEntry(d, 0, 1, -1) == 0 && d.families[0].entries[0].file == "Fonts/Two.ttf", "move a face earlier");
        check(fsMoveEntry(d, 0, 0, -1) == -1, "the top of the chain cannot move up");
        check(fsMoveEntry(d, 0, 1, 1) == -1, "a face cannot move past the OS fonts");
        check(fsMoveEntry(d, 0, 1, 0) == -1, "a zero move is refused");
        check(grouped(d.families[0]), "still grouped after the moves");
        check(fsRemoveEntry(d, 0, 0) && d.families[0].entries[0].file == "Fonts/One.ttf", "remove an entry");
        check(!fsRemoveEntry(d, 0, 9), "remove a bad entry is refused");
    }

    AVER_INFO("attributes");
    {
        UiFontSetDoc d;
        fsAddFamily(d, "A");
        fsAddFace(d, 0, "Fonts/One.ttf");
        check(fsSetAttr(d, 0, 0, "weight", "bold") && d.families[0].entries[0].bold, "weight bold");
        check(fsSetAttr(d, 0, 0, "weight", "regular") && !d.families[0].entries[0].bold, "weight regular");
        check(!fsSetAttr(d, 0, 0, "weight", "heavy"), "an unknown weight is refused");
        check(fsSetAttr(d, 0, 0, "italic", "true") && d.families[0].entries[0].italic, "italic true");
        check(fsSetAttr(d, 0, 0, "italic", "0") && !d.families[0].entries[0].italic, "italic 0");
        check(!fsSetAttr(d, 0, 0, "italic", "maybe"), "a bad bool is refused");
        check(fsSetAttr(d, 0, 0, "index", "2") && d.families[0].entries[0].index == 2, "index");
        check(!fsSetAttr(d, 0, 0, "index", "-1") && !fsSetAttr(d, 0, 0, "index", "x"), "a bad index is refused");
        check(fsSetAttr(d, 0, 0, "lang", "ja") && d.families[0].entries[0].lang == "ja", "lang");
        check(fsSetAttr(d, 0, 0, "lang", "") && d.families[0].entries[0].lang.empty(), "an empty lang clears it");
        check(fsSetAttr(d, 0, 0, "scale", "1.25") && d.families[0].entries[0].scale == 1.25f, "scale");
        check(!fsSetAttr(d, 0, 0, "scale", "0") && !fsSetAttr(d, 0, 0, "scale", "-2") && d.families[0].entries[0].scale == 1.25f,
              "a non-positive scale is refused and leaves the entry alone");
        check(fsSetAttr(d, 0, 0, "dy", "-1.5") && d.families[0].entries[0].dy == -1.5f, "dy");
        check(!fsSetAttr(d, 0, 0, "dy", "1.5px"), "trailing junk in a number is refused");
        check(fsSetAttr(d, 0, 0, "file", "Fonts/Two.ttf") && d.families[0].entries[0].file == "Fonts/Two.ttf", "file");
        check(!fsSetAttr(d, 0, 0, "file", ""), "an empty file is refused");
        check(!fsSetAttr(d, 0, 0, "colour", "red"), "an unknown key is refused");
        check(!fsSetAttr(d, 0, 4, "dy", "1"), "a bad entry is refused");
        check(fsSetLineGap(d, 0, 1.2f) && d.families[0].lineGap == 1.2f, "line gap");
        check(!fsSetLineGap(d, 0, -1.0f), "a negative line gap is refused");

        // The document survives a write and a read.
        const UiFontSetDoc back = parsed(UiFontSet::write(d));
        const UiFontEntry& e = back.families[0].entries[0];
        check(e.file == "Fonts/Two.ttf" && e.index == 2 && e.scale == 1.25f && e.dy == -1.5f && back.families[0].lineGap == 1.2f,
              "edited attributes round-trip through the text format");
    }

    AVER_INFO("overrides and validation");
    {
        UiFontSetDoc d;
        fsAddFamily(d, "Body");
        fsAddFamily(d, "BodyJa");
        check(fsAddOverride(d, "ja", "body", "bodyja") == 0 && d.overrides[0].from == "Body" && d.overrides[0].to == "BodyJa",
              "an override resolves family names ignoring case");
        check(fsAddOverride(d, "JA", "Body", "Body") == 0 && d.overrides.size() == 1 && d.overrides[0].to == "Body",
              "the same language and source replaces the row");
        check(fsAddOverride(d, "ko", "Body", "BodyJa") == 1, "a different language adds a row");
        check(fsAddOverride(d, "ja", "Nope", "Body") == -1 && fsAddOverride(d, "ja", "Body", "Nope") == -1, "an unknown family is refused");
        check(fsAddOverride(d, "", "Body", "BodyJa") == -1, "an empty language is refused");
        check(fsRemoveOverride(d, 0) && d.overrides.size() == 1 && !fsRemoveOverride(d, 5), "remove an override");

        UiFontSetDoc bad = d;
        bad.defaultFamily = "Ghost";
        check(fsValidate(bad).find("DEFAULT") == 0, "a default that names no family is reported");
        bad = d;
        bad.families[1].name = "body";
        check(fsValidate(bad).find("two families") == 0, "duplicate names are reported");
        bad = d;
        bad.overrides.push_back(UiFontOverride{"ja", "Body", "Ghost"});
        check(fsValidate(bad).find("OVERRIDE") == 0, "an override to nowhere is reported");
        bad = d;
        bad.families[0].entries.push_back(UiFontEntry{});
        check(!fsValidate(bad).empty(), "a face with no file is reported");
    }

    AVER_INFO("content root");
    {
        check(fontContentRootOf("C:/proj/Content/UI/Fonts.ocfonts") == "C:/proj/Content", "the Content folder of an asset");
        check(fontContentRootOf("C:\\proj\\content\\UI\\Fonts.ocfonts") == "C:/proj/content", "backslashes and case");
        check(fontContentRootOf("C:/proj/Content/A/Content/B/x.ocfonts") == "C:/proj/Content/A/Content", "the last Content folder");
        check(fontContentRootOf("C:/loose/x.ocfonts") == "C:/loose", "no Content folder: the file's own folder");
    }

    const std::string dir = (std::filesystem::temp_directory_path() / "aver-fontset-editor").string();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const std::string content = dir + "/Content";
    std::filesystem::create_directories(content + "/UI", ec);
    std::filesystem::create_directories(content + "/Fonts", ec);

    testfont::Spec spec = testfont::simpleSpec();
    spec.kern.push_back(testfont::KernPair{'A', 'V', -80});
    const std::vector<u8> fontBytes = testfont::buildFont(spec);
    writeBytes(content + "/Fonts/Test.ttf", fontBytes);

    AVER_INFO("coverage");
    {
        UiFontFace face;
        UiFontBlob blob;
        blob.data = fontBytes.data();
        blob.size = fontBytes.size();
        check(face.open(blob, 0), "the synthetic font opens");
        const FontCoverage c = fsCoverage(face);
        check(c.glyphs == face.glyphCount() && c.glyphs > 0, "the glyph count");
        check(c.kern == UiKernSource::Kern, "kerning from the kern table");
        u32 basic = 0, greek = 0, cjk = 0;
        for (const FontBlockCoverage& b : c.blocks) {
            if (std::string(b.name) == "Basic Latin") basic = b.present;
            if (std::string(b.name) == "Greek") greek = b.present;
            if (std::string(b.name) == "CJK Unified Ideographs") cjk = b.present;
        }
        check(basic == 63, "63 of the 95 Basic Latin code points (A-Z a-z 0-9 space)");
        check(greek == 0 && cjk == 0, "no Greek and no CJK");
        check(c.arabicFormsPresent == 0 && c.arabicFormsTotal == 141, "no Arabic presentation forms of 141");
    }

    AVER_INFO("the tab: load, edit, undo, save");
    {
        const std::string path = content + "/UI/Fonts.ocfonts";
        const std::string text =
            "OCFONTS 1\nDEFAULT Test\n\nFAMILY Test\nFACE Fonts/Test.ttf\nBITMAP legacy\n";
        check(writeFileText(path, text), "the fixture writes");

        FontSetEditor ed(path);
        check(ed.loaded() && ed.loadError().empty(), "the tab loads the file");
        check(ed.title().find("Fonts.ocfonts") == 0 && ed.title().find("###fonts:") != std::string::npos, "title with a stable id");
        check(!ed.dirty() && !ed.canUndo(), "clean, nothing to undo");

        check(ed.addFamily("Heading") == 1 && ed.selectedFamily() == 1, "add a family and select it");
        check(ed.dirty() && ed.canUndo(), "dirty with an undo entry");
        check(ed.addFace("Fonts/Heading.ttf") == 0, "add a face to the selected family");
        check(ed.setAttr(0, "weight", "bold") && ed.doc().families[1].entries[0].bold, "set an attribute");
        check(!ed.setAttr(0, "scale", "0"), "a refused edit says so");
        check(ed.setLineGap(1.4f), "line gap");
        check(ed.addOverride("ja", "Test", "Heading") == 0, "override");
        const u32 v = ed.version();
        ed.undo();
        check(ed.doc().overrides.empty() && ed.version() != v, "undo the override and bump the version");
        ed.undo();
        ed.undo();
        check(ed.doc().families[1].entries.size() == 1 && !ed.doc().families[1].entries[0].bold, "undo to just after the face");
        ed.redo();
        check(ed.doc().families[1].entries[0].bold, "redo");

        check(ed.removeFamily(1) && ed.doc().families.size() == 1 && ed.selectedFamily() == 0, "remove the selected family");
        check(!ed.removeFamily(0), "the last family stays");
        check(ed.addFamily("Heading") == 1, "add it again");
        check(ed.addSystem() == 0, "add the OS fonts");
        check(ed.addBitmap() >= 0, "add the bitmap font");
        check(ed.moveEntry(1, -1) == -1, "a move across groups is refused");

        std::string why;
        check(ed.save(&why), "the tab saves: " + why);
        check(!ed.dirty(), "clean after saving");
        std::string onDisk;
        check(readFileText(path, onDisk), "read it back");
        const UiFontSetDoc back = parsed(onDisk);
        check(back.families.size() == 2 && back.families[1].name == "Heading" && fsValidate(back).empty(), "the saved file parses and validates");
        check(back.families[1].entries.size() == 2, "the OS fonts and the bitmap font were saved");

        // A clean tab follows the file; a dirty tab keeps its edits.
        check(writeFileText(path, text), "rewrite the file on disk");
        ed.onFileChanged();
        check(ed.doc().families.size() == 1 && !ed.dirty() && !ed.canUndo(), "a clean tab reloads");
        ed.addFamily("Mine");
        check(writeFileText(path, text), "rewrite again");
        ed.onFileChanged();
        check(ed.dirty() && ed.doc().families.size() == 2, "a dirty tab keeps its edits");
    }

    AVER_INFO("a file that cannot be saved or loaded");
    {
        const std::string path = content + "/UI/Bad.ocfonts";
        check(writeFileText(path, "OCFONTS 1\nFAMILY A\nFACE Fonts/Test.ttf\nOVERRIDE ja A Ghost\n"), "a set with an override to nowhere");
        FontSetEditor ed(path);
        check(ed.loaded(), "it loads (the runtime tolerates it)");
        std::string why;
        check(!ed.save(&why) && why.find("OVERRIDE") == 0, "saving it is refused with the reason");

        const std::string junk = content + "/UI/Junk.ocfonts";
        check(writeFileText(junk, "not a font set\n"), "a file that is not a font set");
        FontSetEditor bad(junk);
        check(!bad.loaded() && !bad.loadError().empty(), "it does not load and says why");
        check(bad.addFamily("X") == -1 && !bad.save(&why), "its edits and its save are refused");

        FontSetEditor missing(content + "/UI/DoesNotExist.ocfonts");
        check(!missing.loaded() && !missing.loadError().empty(), "a missing file does not load");

        check(makeFontSetEditor(content + "/UI/Bad.ocfonts") != nullptr, "the factory takes .ocfonts");
        check(makeFontSetEditor(content + "/UI/Bad.ocui") == nullptr, "and nothing else");
    }

    AVER_INFO("the preview engine");
    {
        const std::string path = content + "/UI/Fonts.ocfonts";
        check(writeFileText(path, "OCFONTS 1\nDEFAULT Test\nFAMILY Test\nFACE Fonts/Test.ttf\n"), "the project's font set");

        EditorTextPreview pv;
        check(pv.ensureLoaded(content), "the project's font set loads from disk and has a TrueType face");
        check(pv.familyNames().size() == 1 && pv.familyNames()[0] == "Test", "the family names");

        UiTextStyle st;
        st.family = "Test";
        st.px = 20.0f;
        pv.beginFrame(1);
        std::shared_ptr<const UiTextLayout> lay = pv.engine().layout("AV", st, UiTextBox{});
        check(lay && lay->glyphs.size() == 2 && !lay->incomplete, "text lays out and completes");
        check(lay && lay->glyphs[0].face != kUiFaceNone && lay->glyphs[0].flags == 0, "from the TrueType face, no missing-glyph box");
        const u16 slot = lay ? lay->glyphs[0].face : kUiFaceNone;
        const FontCoverage* cov = pv.coverageOf(slot);
        check(cov && cov->glyphs > 0, "coverage by slot");
        check(pv.coverageOf(kUiFaceBitmap) == nullptr, "no coverage for the bitmap face");
        check(pv.engine().atlas().pageCount() >= 1, "glyphs went into the atlas");

        // The tab previews its unsaved edits without touching the project's engine.
        UiFontSetDoc doc = parsed("OCFONTS 1\nFAMILY Test\nFACE Fonts/Test.ttf\n");
        fsAddFamily(doc, "Second");
        EditorTextPreview other;
        check(other.loadDoc(doc, content), "an explicit document loads");
        check(other.familyNames().size() == 2, "with its unsaved family");
        check(pv.familyNames().size() == 1, "the other engine is untouched");

        // A reload follows the same root without reading the disk again, until invalidated.
        check(pv.ensureLoaded(content), "ensureLoaded on the same root is cheap");
        pv.invalidate();
        check(pv.ensureLoaded(content), "and reloads after invalidate");

        UiFontSetDoc nofont = parsed("OCFONTS 1\nFAMILY Test\nFACE Fonts/Missing.ttf\n");
        EditorTextPreview none;
        std::string err;
        none.loadDoc(nofont, content, &err);
        check(!none.warnings().empty() || !none.usable(), "a face that cannot be opened is reported or leaves nothing usable");
    }

    std::filesystem::remove_all(dir, ec);
    if (g_failures) {
        AVER_ERROR("FontSetEditorTest: {} failure(s)", g_failures);
        return 1;
    }
    AVER_INFO("FontSetEditorTest: all passed");
    return 0;
}
