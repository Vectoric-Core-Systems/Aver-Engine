// The Texture editor tab. See the header for the two-line SandboxApp hook and for why this is
// read-only.

#include "TextureEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/Image.hpp"
#include "aver/runtime/Engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

namespace {

// Lower-cases a path's extension, dot included -- the same tiny loop every make*Editor factory in
// this tree repeats rather than shares (BtEditor/SoundEditor/AssetEditor.cpp's own makeMeshEditor).
std::string extensionLower(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// Guards onFileChanged() against a delete-then-recreate race the watcher can report as one event.
bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

// A human name for the container, for the Details panel -- display only, independent of whatever
// undecodableContainer() (Image.hpp) would say about the BYTES, since this tab only ever opens a
// file whose extension already passed makeTextureEditor's own filter.
const char* containerName(const std::string& ext) {
    if (ext == ".png") return "PNG";
    if (ext == ".jpg" || ext == ".jpeg") return "JPEG";
    if (ext == ".tga") return "TGA";
    if (ext == ".bmp") return "BMP";
    if (ext == ".hdr") return "Radiance HDR";
    return "Unknown";
}

// 12345678 -> "11.77 MB". kUnits stops at GB because a source image reaching one is not a case this
// viewer needs to make especially readable.
std::string formatBytes(u64 bytes) {
    static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; ++u; }
    char buf[64];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.2f %s", v, kUnits[u]);
    return buf;
}

#if AVER_WITH_IMGUI
constexpr f32 kMinZoom = 0.02f;
constexpr f32 kMaxZoom = 32.0f;

// Alternating squares clipped to [p0,p1), fixed in SCREEN space rather than image space -- the cell
// size does not grow with zoom, matching every other checkerboard-behind-alpha convention (this
// tree has none of its own yet to match; this is the common one).
void drawCheckerboard(ImDrawList* dl, ImVec2 p0, ImVec2 p1, f32 dpi) {
    const f32 cell = 8.0f * dpi;
    if (cell <= 0.0f) return;
    const ImU32 light = IM_COL32(96, 96, 100, 255);
    const ImU32 dark  = IM_COL32(64, 64, 68, 255);
    int row = 0;
    for (f32 y = p0.y; y < p1.y; y += cell, ++row) {
        int col = row;
        for (f32 x = p0.x; x < p1.x; x += cell, ++col) {
            const ImVec2 a(x, y);
            const ImVec2 b(std::min(x + cell, p1.x), std::min(y + cell, p1.y));
            dl->AddRectFilled(a, b, (col & 1) ? light : dark);
        }
    }
}
#endif

} // namespace

TextureEditor::TextureEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

TextureEditor::~TextureEditor() { releaseTexture(); }

// Decodes the file and gathers its stats. See the header for why this has no ImGui or device
// dependency: aver::decodeImage is stb_image, pure CPU, and runs the same whether or not there is a
// window to show the result in.
void TextureEditor::loadFromDisk() {
    releaseTexture();     // a reload replaces whatever GPU copy the previous version had
    uploadTried_ = false;
    pixels_.clear();

    ImageData img;
    std::string why;
    if (!decodeImage(path_, img, &why)) {
        loaded_ = false;
        loadError_ = why;
        width_ = height_ = srcChannels_ = 0;
        fileBytes_ = 0;
        return;
    }

    loaded_ = true;
    loadError_.clear();
    width_ = img.width;
    height_ = img.height;
    srcChannels_ = img.channels;
    pixels_ = std::move(img.pixels);

    std::error_code ec;
    fileBytes_ = std::filesystem::file_size(path_, ec);
    if (ec) fileBytes_ = 0;

    fitRequested_ = true;   // a reloaded image (different size, maybe) earns a fresh Fit
}

// Reflects an external edit -- an artist re-saving the file in another program while this tab is
// open. UNCONDITIONAL, unlike SoundEditor's own onFileChanged: there is no authored, in-memory edit
// here that a reload could ever discard (dirty() is always false), so the "keep the tab's own
// edits" guard SoundEditor needs has nothing to guard.
void TextureEditor::onFileChanged() {
    if (!fileExists(path_)) return;   // a delete-then-recreate race; the next watch tick catches it
    loadFromDisk();
}

std::string TextureEditor::title() const {
    return std::filesystem::path(path_).filename().string() + "  [Texture]";
}

// Uploads the decoded pixels the first time a device is reachable. See the header: this needs no
// AVER_WITH_IMGUI guard on its own account (nothing here is an ImGui call), only draw() below
// actually reaches it.
void TextureEditor::ensureTexture(Engine& e) {
    if (uploadTried_ || pixels_.empty() || width_ == 0 || height_ == 0) return;
    uploadTried_ = true;

    rhi::IDevice* dev = e.device();
    if (!dev) return;
    rhi::IResourceFactory* res = dev->resources();
    if (!res) return;

    rhi::TextureDesc td;
    td.dim = rhi::TextureDim::Tex2D;
    td.width = width_;
    td.height = height_;
    td.mips = 1;
    td.format = rhi::Format::RGBA8Unorm;   // matches decodeImage's straight (non-sRGB) bytes
    td.bind = rhi::ResourceBind::ShaderResource;
    td.initialState = rhi::ResourceState::ShaderResource;
    td.debugName = "TextureEditor.Preview";
    const void* levels[1] = {pixels_.data()};
    td.initialData = levels;
    td.initialDataCount = 1;
    td.initialRowPitch = width_ * 4;
    tex_ = res->createTexture(td);
    if (!tex_) {
        AVER_WARN("[TextureEditor] '{}' could not be uploaded for preview", path_);
        pixels_.clear();
        pixels_.shrink_to_fit();
        return;
    }

    dev_ = dev;
    uiId_ = dev->uiTextureId(tex_);
    if (!uiId_) AVER_WARN("[TextureEditor] '{}' preview texture is not reachable from the UI", path_);

    // ON THE GPU NOW: the CPU copy has no further job, and freeing it here (rather than at the end
    // of the tab's life) matters for exactly the assets this tab is for -- a 4K source is 64 MB of
    // decoded RGBA8 that would otherwise sit resident twice for as long as the tab stays open.
    pixels_.clear();
    pixels_.shrink_to_fit();
}

void TextureEditor::releaseTexture() {
    if (dev_ && tex_) {
        if (rhi::IResourceFactory* res = dev_->resources()) res->destroyTexture(tex_);
    }
    tex_ = 0;
    uiId_ = 0;
    dev_ = nullptr;
}

#if AVER_WITH_IMGUI

namespace {
constexpr f32 kDefaultCanvasFraction = 0.72f;   // the viewport is "most of the tab" -- see the brief
constexpr const char* kPrefCanvasSplit = "textureEditor.canvasSplit";
} // namespace

void TextureEditor::drawToolbar(f32 dpi) {
    if (ImGui::Button("Fit")) fitRequested_ = true;
    ImGui::SameLine();
    if (ImGui::Button("100%")) { zoom_ = 1.0f; panX_ = panY_ = 0.0f; fitRequested_ = false; }
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f%%", zoom_ * 100.0f);

    ImGui::SameLine(0.0f, 20.0f * dpi);
    ImGui::Checkbox("R", &showR_);
    ImGui::SameLine();
    ImGui::Checkbox("G", &showG_);
    ImGui::SameLine();
    ImGui::Checkbox("B", &showB_);
    ImGui::SameLine();
    ImGui::Checkbox("Alpha blend", &alphaBlend_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Blends the image over the checkerboard using its real alpha (off shows\n"
                          "a solid backdrop instead). There is no cheap way to view the alpha\n"
                          "channel itself as a greyscale image without a dedicated shader, so\n"
                          "this read-only viewer does not add one.");
}

void TextureEditor::drawCanvas(f32 dpi) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##texcanvas", ImVec2(std::max(avail.x, 40.0f), std::max(avail.y, 40.0f)));
    const ImVec2 rectMin = ImGui::GetItemRectMin();
    const ImVec2 rectMax = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(rectMin, rectMax, true);
    dl->AddRectFilled(rectMin, rectMax, IM_COL32(30, 30, 34, 255));

    const ImVec2 centre((rectMin.x + rectMax.x) * 0.5f, (rectMin.y + rectMax.y) * 0.5f);

    if (fitRequested_ && width_ > 0 && height_ > 0) {
        const f32 fit = std::min((rectMax.x - rectMin.x) / static_cast<f32>(width_),
                                  (rectMax.y - rectMin.y) / static_cast<f32>(height_));
        zoom_ = std::clamp(fit, kMinZoom, kMaxZoom);
        panX_ = panY_ = 0.0f;
        fitRequested_ = false;
    }

    if (uiId_ != 0 && width_ > 0 && height_ > 0) {
        const f32 imgW = static_cast<f32>(width_) * zoom_;
        const f32 imgH = static_cast<f32>(height_) * zoom_;
        const ImVec2 p0(centre.x + panX_ - imgW * 0.5f, centre.y + panY_ - imgH * 0.5f);
        const ImVec2 p1(p0.x + imgW, p0.y + imgH);

        if (alphaBlend_) drawCheckerboard(dl, p0, p1, dpi);
        else              dl->AddRectFilled(p0, p1, IM_COL32(90, 90, 94, 255));

        const ImVec4 tint(showR_ ? 1.0f : 0.0f, showG_ ? 1.0f : 0.0f, showB_ ? 1.0f : 0.0f, 1.0f);
        dl->AddImage(static_cast<ImTextureID>(uiId_), p0, p1, ImVec2(0, 0), ImVec2(1, 1),
                    ImGui::ColorConvertFloat4ToU32(tint));
    } else {
        const char* msg = tex_ ? "Waiting for the GPU upload..." : "No preview on this backend.";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f),
                    IM_COL32(160, 160, 168, 255), msg);
    }
    dl->PopClipRect();

    // HOVER-GATED, NOT ACTIVE-GATED, matching AssetEditor.cpp's MeshEditor::drawPreview -- the same
    // convention this codebase already uses for an orbit-drag over a preview image.
    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            const f32 newZoom = std::clamp(zoom_ * std::pow(1.1f, io.MouseWheel), kMinZoom, kMaxZoom);
            // Zoom AROUND THE CURSOR: solve panX_/panY_ so the image point currently under the mouse
            // stays under it after the zoom changes, the same shape as GraphEditorGeometry's own
            // zoomAroundScreenPoint (not shared here -- that one is keyed to a node-graph
            // CanvasTransform this tab has no reason to depend on).
            const f32 px = (io.MousePos.x - centre.x - panX_) / zoom_;
            const f32 py = (io.MousePos.y - centre.y - panY_) / zoom_;
            panX_ = io.MousePos.x - centre.x - px * newZoom;
            panY_ = io.MousePos.y - centre.y - py * newZoom;
            zoom_ = newZoom;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            panX_ += d.x;
            panY_ += d.y;
        }
    }
}

void TextureEditor::drawDetails() {
    ImGui::TextDisabled("%s", path_.c_str());
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Image", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Resolution        %u x %u", width_, height_);
        ImGui::Text("Container         %s", containerName(extensionLower(path_)));
        ImGui::Text("Source channels   %u", srcChannels_);
        ImGui::Text("Decoded as        RGBA8");
    }
    ImGui::TextDisabled("(this engine expands every source image to RGBA8 on decode)");

    if (ImGui::CollapsingHeader("Storage", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("File size         %s", formatBytes(fileBytes_).c_str());
        ImGui::Text("Mip levels        1 (not mipmapped)");
        const u64 gpuBytes = static_cast<u64>(width_) * static_cast<u64>(height_) * 4ull;
        ImGui::Text("Approx GPU memory %s", formatBytes(gpuBytes).c_str());
    }

    if (!tex_) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "No GPU preview on this backend.");
    }
}

void TextureEditor::draw(Engine& e) {
    if (!loaded_) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "This file could not be read.");
        ImGui::Separator();
        ImGui::TextWrapped("%s", loadError_.c_str());
        return;
    }
    ensureTexture(e);

    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    drawToolbar(dpi);
    ImGui::Separator();

    const f32 avail = ImGui::GetContentRegionAvail().x;
    const f32 paneH = ImGui::GetContentRegionAvail().y;
    const f32 minCanvas = 200.0f * dpi, minDetails = 200.0f * dpi;
    const f32 canvasW = splitPaneWidth(split_, kPrefCanvasSplit, kDefaultCanvasFraction, avail,
                                       minCanvas, minDetails);
    if (ImGui::BeginChild("##texcanvaspane", ImVec2(canvasW, paneH), true)) drawCanvas(dpi);
    ImGui::EndChild();
    drawSplitHandle(split_, "##texsplit", kPrefCanvasSplit, avail, minCanvas, minDetails, 6.0f * dpi);
    if (ImGui::BeginChild("##texdetails", ImVec2(0, paneH), true)) drawDetails();
    ImGui::EndChild();
}

void TextureEditor::resetLayout() {
    resetSplitPane(split_, kPrefCanvasSplit, kDefaultCanvasFraction);
}

#else   // AVER_WITH_IMGUI

// The headless build (and tests/editor's target) still gets load/decode/stats through
// loadFromDisk()/onFileChanged(); only the window is absent -- matching SoundEditor/MeshEditor.
void TextureEditor::draw(Engine& e) { (void)e; }
void TextureEditor::resetLayout() {}

#endif  // AVER_WITH_IMGUI

// Editor factory: only the extensions aver::decodeImage (stb_image, Image.hpp) actually decodes.
//
// .DDS IS DELIBERATELY NOT HERE despite being a texture container this engine ships cooked ones in
// elsewhere. stb_image cannot read it -- Image.hpp's own undecodableContainer() explicitly refuses
// the "DDS " magic -- and there is no second, DDS-capable loader in this tree (checked: no other
// decoder exists outside Image.cpp's one stb_image translation unit). Registering this factory for
// .dds would open a tab that unconditionally fails to load every file it claims to handle. The list
// below is kept in step with SandboxApp::isTextureSource (SandboxAutosave.cpp) restricted to the
// container types actually named in this feature's brief; isTextureSource itself additionally
// accepts psd/gif/pic/ppm/pgm/octex, which nothing has asked this tab to claim.
std::unique_ptr<AssetEditor> makeTextureEditor(const std::string& path) {
    const std::string ext = extensionLower(path);
    if (ext != ".png" && ext != ".tga" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp" &&
        ext != ".hdr")
        return nullptr;
    return std::make_unique<TextureEditor>(path);
}

} // namespace aver::editor
