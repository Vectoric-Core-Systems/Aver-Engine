#pragma once
// The Texture editor tab: a read-only viewer for a source image file (.png/.tga/.jpg/.jpeg/.bmp/
// .hdr -- see makeTextureEditor's own comment for why .dds is not on that list even though it is a
// texture container). Shown centred on a checkerboard so alpha reads correctly, with Fit/100% zoom,
// wheel-zoom and drag-pan, R/G/B toggles and a Details panel (resolution, format, file size, GPU
// memory).
//
// THE HOOK INTO SandboxApp.cpp IS TWO LINES, matching every other tab registered there:
//   #include "TextureEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeTextureEditor);   // APPENDED -- order is precedence
// and nothing else.
//
// READ-ONLY, DELIBERATELY: there is no authored data here to edit -- a source image is bytes on
// disk this tab decodes and displays, not a format this engine owns. dirty() therefore keeps
// AssetEditor's default (false) and save() keeps AssetEditor's default (a no-op success), so this
// tab never appears in "unsaved changes" prompts.
#include "AssetEditor.hpp"
#include "EditorWidgets.hpp"   // SplitPane -- ImGui-free except its own guarded half; see its header

#include "aver/rhi/RHI.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver::editor {

// Declared in the header rather than hidden behind the factory, matching SoundEditor/MeshEditor:
// tests/editor can construct the tab directly to exercise the decode/stat path with no ImGui and no
// window, and the .cpp compiles this class either way -- see the class's own comments for exactly
// which members and methods that split touches.
class TextureEditor final : public AssetEditor {
public:
    explicit TextureEditor(std::string path);
    ~TextureEditor() override;

    const std::string& path() const override { return path_; }
    std::string title() const override;
    void draw(Engine& e) override;
    void resetLayout() override;
    void onFileChanged() override;

    // Reachable for a headless test, the same reason SoundEditor exposes loaded()/loadError().
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    u32 imageWidth() const { return width_; }
    u32 imageHeight() const { return height_; }
    u32 sourceChannels() const { return srcChannels_; }
    u64 fileBytes() const { return fileBytes_; }

private:
    // Decodes the file and gathers its stats. Pure CPU (aver::decodeImage is stb_image), so this
    // runs from the constructor and from onFileChanged() with no device and no ImGui context.
    // Leaves `pixels_` populated on success; ensureTexture() below consumes and clears it once the
    // bytes are on the GPU.
    void loadFromDisk();

    // Uploads `pixels_` to the GPU the first time a device is available, then frees the CPU copy --
    // the decoded RGBA8 buffer and the GPU texture would otherwise both be resident for the life of
    // the tab for no reason. Idempotent: a second call with nothing new to upload is a no-op.
    void ensureTexture(Engine& e);
    // Destroys the GPU texture this tab owns, if any. Called by the destructor and by
    // loadFromDisk() ahead of a reload -- NEITHER of those is behind AVER_WITH_IMGUI (a headless
    // build's tab never creates a texture in the first place, so this is just always-safe cleanup).
    void releaseTexture();

    std::string path_;
    bool loaded_ = false;
    std::string loadError_;

    // Decoded stats, kept after the pixels themselves are freed -- see ensureTexture()'s comment.
    u32 width_ = 0, height_ = 0;
    u32 srcChannels_ = 0;         // what the FILE held, 1..4; decodeImage always expands to RGBA
    std::vector<u8> pixels_;      // RGBA8, tightly packed; cleared once uploaded
    u64 fileBytes_ = 0;

    rhi::IDevice* dev_ = nullptr;      // the device the GPU texture below belongs to, or null
    rhi::TextureHandle tex_ = 0;
    u64 uiId_ = 0;
    bool uploadTried_ = false;

    // View state -- plain floats, not ImVec2, so this class needs no ImGui type in its header (the
    // same reason SoundEditor's own fields stay ImGui-free; only its .cpp includes imgui.h).
    f32 zoom_ = 1.0f;
    f32 panX_ = 0.0f, panY_ = 0.0f;
    bool fitRequested_ = true;    // computes an initial Fit the first time the canvas has a size
    bool showR_ = true, showG_ = true, showB_ = true;
    bool alphaBlend_ = true;      // checkerboard (true) or a solid backdrop (false)

    // The canvas/details split -- a plain SplitPane (EditorWidgets.hpp), matching SoundEditor's own
    // list/details divider. Declared unconditionally, like SoundEditor's own split_: SplitPane has
    // no ImGui type in it, only splitPaneWidth()/drawSplitHandle() (called from draw(), below) do.
    SplitPane split_;

#if AVER_WITH_IMGUI
    void drawToolbar(f32 dpi);
    void drawCanvas(f32 dpi);
    void drawDetails();
#endif
};

// Creates a texture editor for a decodable source image, else nullptr.
std::unique_ptr<AssetEditor> makeTextureEditor(const std::string& path);

} // namespace aver::editor
