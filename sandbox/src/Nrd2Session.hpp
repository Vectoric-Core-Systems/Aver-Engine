#pragma once
// Tools > Train Neural Denoiser... and --nrd2-train: drives render::denoise::Nrd2Trainer (NRD2 phase 4,
// docs/rendering/NRD2.md) from the editor. The trainer records its GPU work in a passive render feature's
// prePass (outside any render pass), so training needs the device but not NRD2 as the active denoiser.
// Progress: a sticky toast with a Cancel action, and this window. Weights go to
// %LOCALAPPDATA%\AverEngine\nrd2_v1.avnn, which the renderer's Nrd2Network reloads when it changes.
#include "aver/core/Types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver::rhi { class IDevice; }

namespace aver::editor {

class Nrd2Session {
public:
    Nrd2Session();
    ~Nrd2Session();
    Nrd2Session(const Nrd2Session&)            = delete;
    Nrd2Session& operator=(const Nrd2Session&) = delete;

    // False in a build without the Voxi module (no NRD2).
    [[nodiscard]] bool available() const;
    void openWindow();
    // --nrd2-train STEPS [DATASETDIR...]: starts on the first tick with a device. No dirs = the default
    // dataset folder (%LOCALAPPDATA%\AverEngine\nrd2_dataset).
    void requestCli(u32 steps, std::vector<std::string> dirs);
    // Once per editor frame, before beginFrame: starts a requested session, adds or removes the GPU hook,
    // updates the toast.
    void tick(rhi::IDevice* dev);
    void drawWindow(f32 dpi);
    void cancel();
    // At editor shutdown, before the device goes: saves the last checkpoint, removes the hook.
    void shutdown();
    [[nodiscard]] bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aver::editor
