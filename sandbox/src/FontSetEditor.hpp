#pragma once
// The Font Set editor tab (.ocfonts) and the private text engine the editors preview with (docs/GAME_UI_TEXT.md
// section 2.14).
//
//   Families   the families, each an ordered fallback chain of project faces, OS fonts and the baked bitmap font
//   Preview    sample strings in Latin, Greek, Cyrillic, Arabic, Hebrew and CJK, a mixed bidi line and a wrap box,
//              with "tint by face" (which face drew each glyph) and the missing-glyph box
//   Coverage   per face: Unicode blocks present, Arabic presentation forms, kerning source
//   Atlas      the preview engine's atlas pages and counters
//
// THE HOOK INTO THE SANDBOX is the same shape as UiLayoutEditor's: in SandboxApp.cpp
//   #include "FontSetEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeFontSetEditor);   // APPENDED -- order is precedence
// plus "New Font Set" in the Content Browser, which writes editor::fontSetStarterText().
//
// Everything that is not drawing is a free function or a member that needs no ImGui, so
// tests/editor/FontSetEditorTest runs it headless (this file's .cpp compiled with AVER_WITH_IMGUI undefined).
#include "AssetEditor.hpp"
#include "SnapshotUndo.hpp"

#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiFontFace.hpp"
#include "aver/ui/UiFontSet.hpp"
#include "aver/ui/UiTextEngine.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ImDrawList;

namespace aver::editor {

// The text the Content Browser's "New Font Set" writes. A valid, loadable .ocfonts (a test parses it).
std::string fontSetStarterText();

// The project's Content folder an asset path lives under: everything up to and including the last "Content"
// directory in `assetPath`, else the file's own folder. Face paths in a font set are relative to it.
std::string fontContentRootOf(const std::string& assetPath);

// ---- the document edits, as FREE FUNCTIONS -----------------------------------------------------------------
//
// Each keeps the document valid (fsValidate "") and the entry order Project, then System, then Bitmap. The tab's own
// members push undo around them.

// Adds a family named `name` (made unique); the first family becomes the default. Returns its index.
i32 fsAddFamily(ui::UiFontSetDoc& d, std::string_view name);
// Removes a family, and every OVERRIDE naming it; the default moves to the first family left. False for a bad index
// or the last family.
bool fsRemoveFamily(ui::UiFontSetDoc& d, i32 index);
// Renames; DEFAULT and the OVERRIDE rows that named the old name follow. False for an empty or taken name.
bool fsRenameFamily(ui::UiFontSetDoc& d, i32 index, std::string_view newName);
// Makes a family the default.
bool fsSetDefault(ui::UiFontSetDoc& d, i32 index);
// Adds a project face (a path relative to Content, or "@exe/Name.ttf") after the family's last project face.
// Returns the entry index.
i32 fsAddFace(ui::UiFontSetDoc& d, i32 family, std::string_view file);
// Adds an OS font by file name ("auto" = the built-in candidates) after the family's last project or OS entry.
i32 fsAddSystem(ui::UiFontSetDoc& d, i32 family, std::string_view file = "auto");
// Adds the BITMAP entry, once per family. Returns its index (the existing one if there already is one).
i32 fsAddBitmap(ui::UiFontSetDoc& d, i32 family);
bool fsRemoveEntry(ui::UiFontSetDoc& d, i32 family, i32 entry);
// Moves an entry one place earlier (delta < 0) or later within its group. Returns its new index, -1 at the end.
i32 fsMoveEntry(ui::UiFontSetDoc& d, i32 family, i32 entry, i32 delta);
// Sets one attribute: file, weight (regular | bold), italic (true | false), index, lang, scale, dy. False for a bad
// key or value, leaving the entry untouched.
bool fsSetAttr(ui::UiFontSetDoc& d, i32 family, i32 entry, std::string_view key, std::string_view value);
bool fsSetLineGap(ui::UiFontSetDoc& d, i32 family, f32 multiplier);
// OVERRIDE <lang> <from> <to>: when the UI language is `lang`, family `from` means `to`. Both must be families.
// An existing row for the same lang and from is replaced. Returns its index, -1 on a bad name.
i32 fsAddOverride(ui::UiFontSetDoc& d, std::string_view lang, std::string_view from, std::string_view to);
bool fsRemoveOverride(ui::UiFontSetDoc& d, i32 index);
// "" when the document can be loaded, else the first problem.
std::string fsValidate(const ui::UiFontSetDoc& d);

// ---- coverage -----------------------------------------------------------------------------------------------

struct FontBlockCoverage {
    const char* name = "";
    u32 first = 0, last = 0;
    u32 present = 0;
    u32 total() const { return last - first + 1; }
};

struct FontCoverage {
    u32 glyphs = 0;
    std::vector<FontBlockCoverage> blocks;
    u32 arabicFormsPresent = 0, arabicFormsTotal = 141;   // FE70..FEFC
    ui::UiKernSource kern = ui::UiKernSource::None;
};

// Which Unicode blocks a face has glyphs for, whether it carries the Arabic presentation forms the shaper needs, and
// where its kerning comes from.
FontCoverage fsCoverage(const ui::UiFontFace& face);

// ---- the preview engine ---------------------------------------------------------------------------------------
//
// A private UiTextEngine the editors preview text with, loaded from a project's Content/UI/Fonts.ocfonts (or the
// built-in default). With ImGui it also keeps the atlas pages on the GPU, uploaded as straight white with the
// coverage as alpha, so ImGui's blend draws them at full strength. It is one per process: the font set editor and
// the UI layout editor share it.
class EditorTextPreview {
public:
    static EditorTextPreview& shared();

    EditorTextPreview();
    ~EditorTextPreview();
    EditorTextPreview(const EditorTextPreview&) = delete;
    EditorTextPreview& operator=(const EditorTextPreview&) = delete;

    ui::UiTextEngine& engine() { return eng_; }
    const ui::UiTextEngine& engine() const { return eng_; }
    // True when a TrueType face is open.
    bool usable() const { return eng_.usable(); }

    // Loads the project's font set when `contentRoot` differs from the last call (or after invalidate()). Cheap
    // otherwise. Returns usable().
    bool ensureLoaded(const std::string& contentRoot);
    // Loads an explicit document: the font set editor previews its unsaved edits.
    bool loadDoc(const ui::UiFontSetDoc& doc, const std::string& contentRoot, std::string* err = nullptr);
    // The next ensureLoaded reloads from disk (a font or .ocfonts changed).
    void invalidate() { root_.clear(); }
    // Starts a frame on the engine (resets the glyph budget). `stamp` identifies the UI frame: calls with the same
    // stamp do nothing, so every editor can call it.
    void beginFrame(u64 stamp);
    // The family names of the loaded set.
    std::vector<std::string> familyNames() const;
    const std::vector<std::string>& warnings() const { return eng_.fontSet().warnings(); }

    // Coverage per open face, cached until the next load.
    const FontCoverage* coverageOf(u16 faceSlot);

#if AVER_WITH_IMGUI
    // Uploads the atlas pages that changed to GPU textures ImGui can draw. Once per ImGui frame (extra calls in the
    // same frame do nothing).
    void syncGpu(Engine& e);
    // ImGui's id for a page's texture, 0 when it has none yet.
    u64 imguiTexture(u32 page) const;
    // Draws a UiDrawList with ImGui: rectangles as filled quads, atlas-page quads as images (skewed ones too), clipped
    // by the list's clip rectangles. (originX, originY) is the screen position of the list's (0, 0), `zoom` scales it.
    void paint(ImDrawList* dl, const ui::UiDrawList& list, f32 originX, f32 originY, f32 zoom);
#endif

private:
    struct Page {
        u32 tex = 0;       // rhi::TextureHandle
        u64 imguiId = 0;
        u32 size = 0;
    };
    struct Retired {
        u32 tex = 0;
        i32 frame = 0;
        void* device = nullptr;
    };
    void retire(u32 tex);
    void uploadDirty();

    ui::UiTextEngine eng_;
    std::string root_;
    bool loadedOnce_ = false;
    u64 stamp_ = ~0ull;
    std::unordered_map<u16, FontCoverage> coverage_;
    std::vector<Page> pages_;
    std::vector<Retired> retired_;
    i32 syncedFrame_ = -1;
    void* device_ = nullptr;   // rhi::IDevice*, to destroy page textures
};

// ---- the tab ---------------------------------------------------------------------------------------------------

class FontSetEditor final : public AssetEditor {
public:
    explicit FontSetEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;
    void resetLayout() override;

    // ---- the headless surface ------------------------------------------------------------------
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const ui::UiFontSetDoc& doc() const { return doc_; }
    i32 selectedFamily() const { return family_; }
    void selectFamily(i32 index);
    void markDirty() { dirty_ = true; ++version_; }
    u32 version() const { return version_; }   // bumps on every change to the document

    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // The edits as the TAB performs them: push undo, apply, keep the selection on what was acted on, mark dirty.
    // -1 / false when the edit refused (and nothing was recorded).
    i32 addFamily(std::string_view name);
    bool removeFamily(i32 index);
    bool renameFamily(i32 index, std::string_view name);
    bool setDefault(i32 index);
    i32 addFace(std::string_view file);              // to the selected family
    i32 addSystem(std::string_view file = "auto");
    i32 addBitmap();
    bool removeEntry(i32 entry);
    i32 moveEntry(i32 entry, i32 delta);
    bool setAttr(i32 entry, std::string_view key, std::string_view value);
    bool setLineGap(f32 multiplier);
    i32 addOverride(std::string_view lang, std::string_view from, std::string_view to);
    bool removeOverride(i32 index);

    // This tab's own preview engine (the unsaved edits), made on first use.
    EditorTextPreview& preview();

private:
    void loadFromDisk();

    std::string path_;
    ui::UiFontSetDoc doc_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    i32 family_ = 0;
    u32 version_ = 0;
    SnapshotUndo<ui::UiFontSetDoc> history_;
    std::unique_ptr<EditorTextPreview> preview_;

#if AVER_WITH_IMGUI
    void drawToolbar();
    void drawFamilies(f32 dpi);
    void drawPreview(Engine& e, f32 dpi);
    void drawCoverage();
    void drawAtlas(f32 dpi);
    // A field changed this frame: remember the document as the frame began (once per drag or typing run) and mark dirty.
    void touched();
    // Ends the run once no field is active: one undo entry for the whole drag or typing.
    void commitEdit();

    u32 previewVersion_ = ~0u;       // the document version the preview engine was loaded with
    bool editing_ = false;
    ui::UiFontSetDoc frameStart_, editSnapshot_;
    f32 previewPx_ = 24.0f;
    f32 wrapWidth_ = 360.0f;
    bool previewBold_ = false, previewItalic_ = false, tint_ = false;
    int previewDir_ = 0;             // 0 auto, 1 ltr, 2 rtl
    char language_[32] = "";
    char newFamily_[64] = "";
    char newOverrideLang_[16] = "ja";
    int overrideFrom_ = 0, overrideTo_ = 0;
    char newFace_[256] = "";
    std::string status_;             // the last save problem, shown in the toolbar
#endif
};

// Creates a font set editor for an .ocfonts, else nullptr.
std::unique_ptr<AssetEditor> makeFontSetEditor(const std::string& path);

} // namespace aver::editor
